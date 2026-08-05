#pragma once

#include <cstdint>

namespace microbrowser::css {

// A CSS length, resolved as far as it can be without a layout context.
//
// Percentages cannot be resolved here — they need a containing block, which
// does not exist until layout runs — so they are carried rather than
// collapsed. A style system that resolved them early would have to guess at the
// containing block, and the guess is wrong for every element inside a float.
struct Length {
  enum class Unit : std::uint8_t { Pixels, Percent, Em, Rem, Auto };

  float value = 0.0f;
  Unit unit = Unit::Pixels;
  // The absolute part of a `calc()` that added pixels to a relative term:
  // `calc(100% - 20px)` is value 100, unit Percent, offset -20. Zero for every
  // length a stylesheet spells without `calc()`, which is why it is an addition
  // to this type rather than a replacement of it — a one-term length still
  // costs one float and one tag to read.
  float offset = 0.0f;

  static Length Auto() { return Length{0.0f, Unit::Auto}; }
  static Length Pixels(float value) { return Length{value, Unit::Pixels}; }

  bool IsAuto() const { return unit == Unit::Auto; }
  bool IsPercent() const { return unit == Unit::Percent; }

  // Absolute pixels, given the font size this length is relative to. Percentage
  // and auto have no answer without a containing block, so they return the
  // fallback the caller supplies rather than silently becoming zero.
  float Resolve(float font_size, float fallback = 0.0f) const;

  // Absolute pixels, given the containing-block extent a percentage is a
  // fraction of. The one place a percentage becomes a number: written out at
  // each call site instead, `calc()`'s offset is the term every one of them
  // forgets.
  float Used(float basis, float font_size) const {
    return unit == Unit::Percent ? basis * value / 100.0f + offset : Resolve(font_size, 0.0f);
  }

  friend bool operator==(const Length&, const Length&) = default;
};

}  // namespace microbrowser::css
