#pragma once

#include <optional>
#include <string_view>

namespace microbrowser::css {

// What a media query is asked about.
//
// Three numbers and no pointer to anything: a media query is a pure function of
// the environment it names, and keeping the environment a value is what lets an
// `<img sizes>` attribute, a `<source media>` attribute and an `@media` prelude
// all be answered by the same evaluator without any of them reaching a document.
struct MediaContext {
  float viewport_width = 0.0f;   // CSS pixels
  float viewport_height = 0.0f;  // CSS pixels
  float device_pixel_ratio = 1.0f;
  // The two `prefers-*` features, and they are **the deliberate exceptions to ADR 0029's constant
  // rule**. Each is one bit of entropy; each materially changes whether a page is usable or
  // comfortable; and paying one bit for that is a better deal than paying it for `deviceMemory`.
  //
  // They are the user's setting rather than the system's, for the reason `Accept-Language` is a
  // constant: reading the desktop environment's theme would make the bit vary by *platform* as well as
  // by preference, which is two bits for one feature.
  bool prefers_dark = false;
  bool prefers_reduced_motion = false;

  // Comparable because `@media` is evaluated at *parse* time (TD-0002), which
  // makes a parsed stylesheet valid only for the context it was parsed in --
  // so anything that caches one has to be able to ask whether the context
  // still matches.
  friend bool operator==(const MediaContext&, const MediaContext&) = default;
};

// Whether a `<media-query-list>` matches: the grammar of an `@media` prelude, of
// a `<source media>` attribute, and -- with the media type omitted -- of the
// condition half of a `sizes` entry. An empty list matches everything, which is
// what makes `sizes="100vw"` a valid entry with no condition in front of it.
//
// Unparsable input is `false` rather than a guess, and so is a feature this
// evaluator does not implement. Both are what the spec says, and the second is
// the one that matters: a media feature is an answer this browser gives about
// the machine it is running on, so every feature added here is a fingerprinting
// decision (ADR 0029) rather than a compatibility one.
bool MediaQueryListMatches(std::string_view text, const MediaContext& context);

// One absolute length, in CSS pixels. `nullopt` when the text is not a single
// length, or is a length in a unit that cannot be resolved without a layout --
// a percentage has no meaning here, because there is no containing block.
//
// Viewport units resolve, unlike in the cascade: `50vw` against a known viewport
// is a number, and the reason css::Length cannot hold one is that it has to
// survive from the cascade to a layout that may happen at a different size.
std::optional<float> ResolveAbsoluteLength(std::string_view text, const MediaContext& context);

// Pixels per unit for the CSS units that need nothing but the unit -- the absolute ones.
//
// **One table, called from three places**, because there were three and they had drifted: the
// cascade's `ParseLength`, `calc()`'s term reader and the media-query dimension reader each carried
// their own `px`/`pt` pair, and none of them had `in`. CSS 2.1's own test suite is written in
// inches -- 1,873 of its files use one -- so `width: 3in` was an invalid declaration and the box it
// sized was 0x0, which renders as a blank page rather than as a wrong length.
//
// Every ratio here is fixed by CSS Values §5.2 against the reference pixel: `1in` is exactly 96px
// by definition, and the rest are defined off the inch, not off a physical measurement of the
// screen. Nothing here reads a DPI, and nothing should.
std::optional<double> AbsoluteUnitScale(std::string_view unit);

// One magnitude in one CSS unit, as absolute pixels. `em` and `rem` fold at
// `kRootFontSize` because there is no element here. Viewport units need a
// non-zero dimension on the axis they read; a zero viewport yields nullopt so
// a length is not guessed before the viewport is known.
std::optional<float> AbsoluteLengthFromUnit(double magnitude, std::string_view unit,
                                            const MediaContext& context);

}  // namespace microbrowser::css
