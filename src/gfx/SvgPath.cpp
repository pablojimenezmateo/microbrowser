#include "gfx/SvgPath.h"

#include <cmath>
#include <cstddef>
#include <string_view>

namespace microbrowser::gfx {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool IsSvgSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// One scan over the data, holding the pen and the state SVG's shorthand
// commands need. A class rather than a pile of by-reference parameters because
// every rule in the grammar reads or writes at least three of them.
class PathDataParser {
 public:
  PathDataParser(std::string_view data, Path& out, std::size_t max_commands)
      : data_(data), out_(&out), remaining_(max_commands) {}

  bool Run();

 private:
  void SkipSpace() {
    while (at_ < data_.size() && IsSvgSpace(data_[at_])) {
      ++at_;
    }
  }

  // A number in SVG's grammar, which is not strtod's: no leading `+`/`-` sign
  // *after* the mantissa, no hex, no infinity, no NaN spelling. Parsed here
  // rather than handed to a general routine so that `1-2` splits into two
  // numbers, which it must -- SVG omits the separator whenever the sign
  // provides one, and every real path takes advantage of it.
  bool Number(float& out);

  // A number that must be 0 or 1, which is how the arc flags are written --
  // and they may be written without any separator at all (`a5 5 0 1150 0`).
  bool Flag(bool& out);

  bool Coordinates(float& x, float& y) { return Number(x) && Number(y); }

  bool Spend() {
    if (remaining_ == 0) {
      return false;
    }
    --remaining_;
    return true;
  }

  // Converts an SVG elliptical arc into up to four cubic segments. The
  // endpoint parameterisation SVG uses has to become the centre one first,
  // which is F.6.5 of the specification; the out-of-range corrections below it
  // (F.6.6) are not optional -- a radius too small for the endpoints is
  // common in hand-written data and must be scaled up rather than rejected.
  bool Arc(float rx, float ry, float x_axis_degrees, bool large_arc, bool sweep, FloatPoint to);

  std::string_view data_;
  Path* out_;
  std::size_t remaining_;
  std::size_t at_ = 0;

  FloatPoint pen_;
  FloatPoint subpath_start_;
  // The reflection source for `S` and `T`. Only valid when the previous
  // command was the matching curve type, which is what `has_*` records: SVG
  // says an unmatched shorthand reflects the current point, i.e. nothing.
  FloatPoint last_cubic_control_;
  FloatPoint last_quad_control_;
  bool has_cubic_control_ = false;
  bool has_quad_control_ = false;
};

bool PathDataParser::Number(float& out) {
  SkipSpace();
  const std::size_t begin = at_;
  if (at_ < data_.size() && (data_[at_] == '+' || data_[at_] == '-')) {
    ++at_;
  }
  bool any_digits = false;
  while (at_ < data_.size() && IsDigit(data_[at_])) {
    ++at_;
    any_digits = true;
  }
  if (at_ < data_.size() && data_[at_] == '.') {
    ++at_;
    while (at_ < data_.size() && IsDigit(data_[at_])) {
      ++at_;
      any_digits = true;
    }
  }
  if (!any_digits) {
    at_ = begin;
    return false;
  }
  if (at_ < data_.size() && (data_[at_] == 'e' || data_[at_] == 'E')) {
    const std::size_t exponent_start = at_;
    ++at_;
    if (at_ < data_.size() && (data_[at_] == '+' || data_[at_] == '-')) {
      ++at_;
    }
    if (at_ < data_.size() && IsDigit(data_[at_])) {
      while (at_ < data_.size() && IsDigit(data_[at_])) {
        ++at_;
      }
    } else {
      // `1e` with no exponent is the number 1 followed by the (invalid)
      // command `e`, not a malformed number.
      at_ = exponent_start;
    }
  }

  // The text is a bounded slice of the attribute, so a stack buffer with a
  // length check is enough and avoids allocating per number in a path with
  // thousands of them.
  const std::size_t length = at_ - begin;
  if (length == 0 || length >= 64) {
    at_ = begin;
    return false;
  }
  char buffer[64];
  for (std::size_t i = 0; i < length; ++i) {
    buffer[i] = data_[begin + i];
  }
  buffer[length] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buffer, &end);
  if (end == buffer || !std::isfinite(value)) {
    at_ = begin;
    return false;
  }
  out = static_cast<float>(value);
  return true;
}

bool PathDataParser::Flag(bool& out) {
  SkipSpace();
  if (at_ >= data_.size() || (data_[at_] != '0' && data_[at_] != '1')) {
    return false;
  }
  out = data_[at_] == '1';
  ++at_;
  return true;
}

bool PathDataParser::Arc(float rx, float ry, float x_axis_degrees, bool large_arc, bool sweep,
                         FloatPoint to) {
  const FloatPoint from = pen_;
  if (from.x == to.x && from.y == to.y) {
    return true;  // a zero-length arc draws nothing, per the specification
  }
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  if (rx == 0.0f || ry == 0.0f) {
    // "If rx or ry is 0, this arc is treated as a straight line."
    out_->LineTo(to);
    pen_ = to;
    return true;
  }

  const float angle = x_axis_degrees * kPi / 180.0f;
  const float cos_a = std::cos(angle);
  const float sin_a = std::sin(angle);
  const float dx2 = (from.x - to.x) * 0.5f;
  const float dy2 = (from.y - to.y) * 0.5f;
  const float x1 = cos_a * dx2 + sin_a * dy2;
  const float y1 = -sin_a * dx2 + cos_a * dy2;

  // F.6.6: grow the radii until they can span the endpoints. Hand-written path
  // data gets this wrong constantly and the correction is what makes it draw.
  const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lambda > 1.0f) {
    const float scale = std::sqrt(lambda);
    rx *= scale;
    ry *= scale;
  }

  const float rx2 = rx * rx;
  const float ry2 = ry * ry;
  const float denominator = rx2 * y1 * y1 + ry2 * x1 * x1;
  if (!(denominator > 0.0f)) {
    out_->LineTo(to);
    pen_ = to;
    return true;
  }
  float factor = (rx2 * ry2 - denominator) / denominator;
  factor = factor > 0.0f ? std::sqrt(factor) : 0.0f;
  if (large_arc == sweep) {
    factor = -factor;
  }
  const float cx1 = factor * rx * y1 / ry;
  const float cy1 = -factor * ry * x1 / rx;
  const FloatPoint center{cos_a * cx1 - sin_a * cy1 + (from.x + to.x) * 0.5f,
                          sin_a * cx1 + cos_a * cy1 + (from.y + to.y) * 0.5f};

  const auto angle_of = [](float ux, float uy, float vx, float vy) {
    const float dot = ux * vx + uy * vy;
    const float length = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    if (!(length > 0.0f)) {
      return 0.0f;
    }
    float value = std::acos(std::fmin(1.0f, std::fmax(-1.0f, dot / length)));
    if (ux * vy - uy * vx < 0.0f) {
      value = -value;
    }
    return value;
  };

  const float start = angle_of(1.0f, 0.0f, (x1 - cx1) / rx, (y1 - cy1) / ry);
  float sweep_angle = angle_of((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx,
                               (-y1 - cy1) / ry);
  if (!sweep && sweep_angle > 0.0f) {
    sweep_angle -= 2.0f * kPi;
  } else if (sweep && sweep_angle < 0.0f) {
    sweep_angle += 2.0f * kPi;
  }

  // A cubic approximates a circular arc well below about a quarter turn and
  // visibly badly above it, so the sweep is split into that many pieces.
  const int segments = static_cast<int>(std::ceil(std::fabs(sweep_angle) / (kPi * 0.5f)));
  const float step = sweep_angle / static_cast<float>(std::max(1, segments));
  const float alpha = std::sin(step) * (std::sqrt(4.0f + 3.0f * std::tan(step * 0.5f) *
                                                             std::tan(step * 0.5f)) -
                                        1.0f) / 3.0f;
  float theta = start;
  for (int i = 0; i < std::max(1, segments); ++i) {
    if (!Spend()) {
      return false;
    }
    const float next = theta + step;
    const auto on_ellipse = [&](float t) {
      const float cos_t = std::cos(t);
      const float sin_t = std::sin(t);
      return FloatPoint{center.x + cos_a * rx * cos_t - sin_a * ry * sin_t,
                        center.y + sin_a * rx * cos_t + cos_a * ry * sin_t};
    };
    const auto derivative = [&](float t) {
      const float cos_t = std::cos(t);
      const float sin_t = std::sin(t);
      return FloatPoint{-cos_a * rx * sin_t - sin_a * ry * cos_t,
                        -sin_a * rx * sin_t + cos_a * ry * cos_t};
    };
    const FloatPoint p0 = on_ellipse(theta);
    const FloatPoint p3 = i + 1 == std::max(1, segments) ? to : on_ellipse(next);
    const FloatPoint d0 = derivative(theta);
    const FloatPoint d3 = derivative(next);
    out_->CubicTo(FloatPoint{p0.x + alpha * d0.x, p0.y + alpha * d0.y},
                  FloatPoint{p3.x - alpha * d3.x, p3.y - alpha * d3.y}, p3);
    theta = next;
  }
  pen_ = to;
  return true;
}

bool PathDataParser::Run() {
  char command = '\0';
  while (true) {
    SkipSpace();
    if (at_ >= data_.size()) {
      return true;
    }

    const char c = data_[at_];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      command = c;
      ++at_;
    } else if (command == '\0') {
      return false;  // data that does not begin with a command
    } else if (command == 'M') {
      // A repeated moveto is a lineto, which is what closes most outlines.
      command = 'L';
    } else if (command == 'm') {
      command = 'l';
    } else if (command == 'Z' || command == 'z') {
      return false;  // numbers after a closepath are not a repeat of anything
    }

    const bool relative = command >= 'a' && command <= 'z';
    const FloatPoint origin = relative ? pen_ : FloatPoint{0.0f, 0.0f};
    const char upper = relative ? static_cast<char>(command - 'a' + 'A') : command;

    if (!Spend()) {
      return false;
    }

    bool cubic_command = false;
    bool quad_command = false;
    switch (upper) {
      case 'M': {
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x, y)) {
          return false;
        }
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        subpath_start_ = pen_;
        out_->MoveTo(pen_);
        break;
      }
      case 'L': {
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x, y)) {
          return false;
        }
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        out_->LineTo(pen_);
        break;
      }
      case 'H': {
        float x = 0.0f;
        if (!Number(x)) {
          return false;
        }
        pen_ = FloatPoint{origin.x + x, pen_.y};
        out_->LineTo(pen_);
        break;
      }
      case 'V': {
        float y = 0.0f;
        if (!Number(y)) {
          return false;
        }
        pen_ = FloatPoint{pen_.x, origin.y + y};
        out_->LineTo(pen_);
        break;
      }
      case 'C': {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x1, y1) || !Coordinates(x2, y2) || !Coordinates(x, y)) {
          return false;
        }
        const FloatPoint c2{origin.x + x2, origin.y + y2};
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        out_->CubicTo(FloatPoint{origin.x + x1, origin.y + y1}, c2, pen_);
        last_cubic_control_ = c2;
        cubic_command = true;
        break;
      }
      case 'S': {
        float x2 = 0.0f;
        float y2 = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x2, y2) || !Coordinates(x, y)) {
          return false;
        }
        // Reflect the previous cubic's second control point. With no previous
        // cubic the reflection is the current point, which makes the first
        // control coincide with the pen -- the specification's wording, and
        // the reason an `S` that starts a subpath still draws something sane.
        const FloatPoint c1 =
            has_cubic_control_ ? FloatPoint{2.0f * pen_.x - last_cubic_control_.x,
                                            2.0f * pen_.y - last_cubic_control_.y}
                               : pen_;
        const FloatPoint c2{origin.x + x2, origin.y + y2};
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        out_->CubicTo(c1, c2, pen_);
        last_cubic_control_ = c2;
        cubic_command = true;
        break;
      }
      case 'Q': {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x1, y1) || !Coordinates(x, y)) {
          return false;
        }
        const FloatPoint control{origin.x + x1, origin.y + y1};
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        out_->QuadTo(control, pen_);
        last_quad_control_ = control;
        quad_command = true;
        break;
      }
      case 'T': {
        float x = 0.0f;
        float y = 0.0f;
        if (!Coordinates(x, y)) {
          return false;
        }
        const FloatPoint control =
            has_quad_control_ ? FloatPoint{2.0f * pen_.x - last_quad_control_.x,
                                           2.0f * pen_.y - last_quad_control_.y}
                              : pen_;
        pen_ = FloatPoint{origin.x + x, origin.y + y};
        out_->QuadTo(control, pen_);
        last_quad_control_ = control;
        quad_command = true;
        break;
      }
      case 'A': {
        float rx = 0.0f;
        float ry = 0.0f;
        float rotation = 0.0f;
        bool large_arc = false;
        bool sweep = false;
        float x = 0.0f;
        float y = 0.0f;
        if (!Number(rx) || !Number(ry) || !Number(rotation) || !Flag(large_arc) ||
            !Flag(sweep) || !Coordinates(x, y)) {
          return false;
        }
        if (!Arc(rx, ry, rotation, large_arc, sweep, FloatPoint{origin.x + x, origin.y + y})) {
          return false;
        }
        break;
      }
      case 'Z': {
        out_->Close();
        // The pen returns to where the subpath began. A relative command after
        // a closepath is relative to *there*, not to where the pen had got to,
        // and getting this wrong displaces every subpath after the first.
        pen_ = subpath_start_;
        break;
      }
      default:
        return false;
    }

    has_cubic_control_ = cubic_command;
    has_quad_control_ = quad_command;
  }
}

}  // namespace

bool ParseSvgPathData(std::string_view data, Path& out, std::size_t max_commands) {
  PathDataParser parser(data, out, max_commands);
  return parser.Run();
}

}  // namespace microbrowser::gfx
