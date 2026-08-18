#include "gfx/Gradient.h"

#include <algorithm>
#include <cmath>

namespace microbrowser::gfx {

namespace {

constexpr float kTwoPi = 6.283185307179586f;

}  // namespace

Paint Paint::Linear(float x0, float y0, float x1, float y1) {
  Paint paint;
  paint.kind_ = Kind::Linear;
  paint.x0_ = x0;
  paint.y0_ = y0;
  paint.x1_ = x1;
  paint.y1_ = y1;
  return paint;
}

Paint Paint::Radial(float x0, float y0, float r0, float x1, float y1, float r1) {
  Paint paint;
  paint.kind_ = Kind::Radial;
  paint.x0_ = x0;
  paint.y0_ = y0;
  paint.r0_ = r0;
  paint.x1_ = x1;
  paint.y1_ = y1;
  paint.r1_ = r1;
  return paint;
}

Paint Paint::Conic(float angle, float cx, float cy) {
  Paint paint;
  paint.kind_ = Kind::Conic;
  paint.x0_ = cx;
  paint.y0_ = cy;
  paint.r0_ = angle;
  return paint;
}

Paint Paint::Pattern(std::shared_ptr<const Image> image, Repeat repeat) {
  Paint paint;
  paint.kind_ = Kind::Pattern;
  paint.image_ = std::move(image);
  paint.repeat_ = repeat;
  return paint;
}

void Paint::AddStop(float offset, Color color) {
  stops_.push_back(ColorStop{offset, color});
  sorted_ = false;
}

void Paint::SetTransform(const AffineTransform& transform) {
  const std::optional<AffineTransform> inverted = transform.Inverted();
  invertible_ = inverted.has_value();
  inverse_ = invertible_ ? *inverted : AffineTransform{};
}

Color Paint::Sample(float t) const {
  if (stops_.empty()) {
    return Color::Rgba(0, 0, 0, 0);
  }
  if (!sorted_) {
    // Stable, because the specification says two stops at the same offset keep the order they were
    // added -- which is how a page draws a hard edge between two colours.
    std::stable_sort(const_cast<std::vector<ColorStop>&>(stops_).begin(),
                     const_cast<std::vector<ColorStop>&>(stops_).end(),
                     [](const ColorStop& a, const ColorStop& b) { return a.offset < b.offset; });
    sorted_ = true;
  }
  if (t <= stops_.front().offset) {
    return stops_.front().color;
  }
  if (t >= stops_.back().offset) {
    return stops_.back().color;
  }
  for (std::size_t i = 1; i < stops_.size(); ++i) {
    if (t <= stops_[i].offset) {
      const ColorStop& from = stops_[i - 1];
      const ColorStop& to = stops_[i];
      const float span = to.offset - from.offset;
      const float local = span > 0.0f ? (t - from.offset) / span : 1.0f;
      // Interpolated in premultiplied space, which is what the specification says and is not a
      // detail: a gradient from opaque red to transparent black interpolated *un*-premultiplied
      // passes through visible grey, which is the classic dark halo.
      const float from_alpha = static_cast<float>(from.color.Alpha()) / 255.0f;
      const float to_alpha = static_cast<float>(to.color.Alpha()) / 255.0f;
      const float alpha = from_alpha + (to_alpha - from_alpha) * local;
      if (alpha <= 0.0f) {
        return Color::Rgba(0, 0, 0, 0);
      }
      const auto channel = [&](std::uint8_t a, std::uint8_t b) {
        const float premultiplied = static_cast<float>(a) * from_alpha +
                                    (static_cast<float>(b) * to_alpha -
                                     static_cast<float>(a) * from_alpha) * local;
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(premultiplied / alpha, 0.0f, 255.0f)));
      };
      return Color::Rgba(channel(from.color.Red(), to.color.Red()),
                         channel(from.color.Green(), to.color.Green()),
                         channel(from.color.Blue(), to.color.Blue()),
                         static_cast<std::uint8_t>(std::lround(alpha * 255.0f)));
    }
  }
  return stops_.back().color;
}

Color Paint::At(float device_x, float device_y) const {
  if (!invertible_) {
    return Color::Rgba(0, 0, 0, 0);
  }
  const FloatPoint user = inverse_.MapPoint(FloatPoint{device_x, device_y});
  switch (kind_) {
    case Kind::Linear: {
      const float dx = x1_ - x0_;
      const float dy = y1_ - y0_;
      const float length_squared = dx * dx + dy * dy;
      if (length_squared == 0.0f) {
        // Two identical points paint nothing at all, per the specification -- not the first stop's
        // colour, which is what a naive `t = 0` would produce.
        return Color::Rgba(0, 0, 0, 0);
      }
      return Sample(((user.x - x0_) * dx + (user.y - y0_) * dy) / length_squared);
    }
    case Kind::Radial: {
      // The specification's construction: find the largest `omega` for which the circle interpolated
      // between the two given circles contains the point, and the gradient position is that omega.
      // Written as the quadratic it reduces to.
      const float cdx = x1_ - x0_;
      const float cdy = y1_ - y0_;
      const float dr = r1_ - r0_;
      const float px = user.x - x0_;
      const float py = user.y - y0_;
      const float a = cdx * cdx + cdy * cdy - dr * dr;
      const float b = px * cdx + py * cdy + r0_ * dr;
      const float c = px * px + py * py - r0_ * r0_;
      float omega = 0.0f;
      if (std::abs(a) < 1e-6f) {
        if (std::abs(b) < 1e-12f) {
          return Color::Rgba(0, 0, 0, 0);
        }
        omega = c / (2.0f * b);
        if (r0_ + omega * dr < 0.0f) {
          return Color::Rgba(0, 0, 0, 0);
        }
      } else {
        const float discriminant = b * b - a * c;
        if (discriminant < 0.0f) {
          return Color::Rgba(0, 0, 0, 0);
        }
        const float root = std::sqrt(discriminant);
        // The larger root first: the specification wants the *largest* omega whose circle has a
        // non-negative radius, and taking the smaller one paints the cone's mirror image.
        const float first = (b + root) / a;
        const float second = (b - root) / a;
        if (r0_ + first * dr >= 0.0f) {
          omega = first;
        } else if (r0_ + second * dr >= 0.0f) {
          omega = second;
        } else {
          return Color::Rgba(0, 0, 0, 0);
        }
      }
      return Sample(omega);
    }
    case Kind::Conic: {
      float angle = std::atan2(user.y - y0_, user.x - x0_) - r0_;
      angle = std::fmod(angle, kTwoPi);
      if (angle < 0.0f) {
        angle += kTwoPi;
      }
      return Sample(angle / kTwoPi);
    }
    case Kind::Pattern: {
      if (image_ == nullptr || image_->Width() <= 0 || image_->Height() <= 0) {
        return Color::Rgba(0, 0, 0, 0);
      }
      const int width = image_->Width();
      const int height = image_->Height();
      // Floor rather than truncate: `static_cast<int>(-0.5)` is 0, so a pattern would repeat its
      // first column twice on either side of the origin.
      int x = static_cast<int>(std::floor(user.x));
      int y = static_cast<int>(std::floor(user.y));
      const bool repeat_x = repeat_ == Repeat::Both || repeat_ == Repeat::X;
      const bool repeat_y = repeat_ == Repeat::Both || repeat_ == Repeat::Y;
      if (repeat_x) {
        x = ((x % width) + width) % width;
      } else if (x < 0 || x >= width) {
        return Color::Rgba(0, 0, 0, 0);
      }
      if (repeat_y) {
        y = ((y % height) + height) % height;
      } else if (y < 0 || y >= height) {
        return Color::Rgba(0, 0, 0, 0);
      }
      const std::uint32_t* row = image_->Row(y);
      return row == nullptr ? Color::Rgba(0, 0, 0, 0) : Color{row[x]};
    }
  }
  return Color::Rgba(0, 0, 0, 0);
}

}  // namespace microbrowser::gfx
