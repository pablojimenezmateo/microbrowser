#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "css/Length.h"

// Everything `background-*` is, as values (ADR 0014).
//
// Its own header rather than more lines in `ComputedStyle.h`, which is at its
// module's cap: a file over its cap means a missing header, not a bigger one --
// the same split `SvgStyle.h` is. It is also the honest one, because only the
// background painter reads any of this and `ComputedStyle` holds it as a single
// grouped member.

namespace microbrowser::css {

struct ComputedStyle;
struct MediaContext;

enum class BackgroundRepeat : std::uint8_t { Repeat, RepeatX, RepeatY, NoRepeat };

// Which of the box model's rectangles a background is measured from, and which
// it is painted into. Two properties, one set of values, so one enum.
//
// **The defaults are not the same box and that is the whole point of having
// both**: `background-origin` is `padding-box`, so a background image starts
// inside the border, and `background-clip` is `border-box`, so what runs past
// the padding edge still shows *under* a dashed or translucent border. Painting
// both from the border box -- which is what this renderer did until the
// property existed -- puts every background image on a bordered element a
// border-width too far up and to the left.
enum class BackgroundBox : std::uint8_t { BorderBox, PaddingBox, ContentBox };

// Everything `background-image` needs beyond the pixels.
//
// One value rather than five fields on ComputedStyle because they are one
// concept: nothing here means anything without `image`, and a shorthand that
// sets the image resets all of them together. Keeping them apart made the
// style struct read as though a page could have a background position with no
// background.
//
// A single layer. CSS allows a list, and a page that writes one gets its first
// image -- see where the shorthand is parsed for why the rest are dropped
// rather than approximated.
struct BackgroundLayer {
  // The `url()`, exactly as the stylesheet wrote it, or empty. Resolving it
  // against the document is the loader's job: the cascade does not know what a
  // base URL is, and doing it in two places is how the two disagree.
  std::string image;
  BackgroundRepeat repeat = BackgroundRepeat::Repeat;
  // `auto` on an axis means the image's own size there, which is what keeps an
  // icon's proportions when a stylesheet gives only a width.
  Length size_x = Length::Auto();
  Length size_y = Length::Auto();
  // A percentage is a fraction of the space the image does *not* fill, which is
  // what makes `50%` centre rather than offset by half the box.
  Length position_x;
  Length position_y;
  // The positioning area and the painting area. Different defaults on purpose;
  // see `BackgroundBox`.
  BackgroundBox origin = BackgroundBox::PaddingBox;
  BackgroundBox clip = BackgroundBox::BorderBox;
  // **`background-color` is clipped by the *bottom-most* layer's clip, and this
  // renderer has one layer.** So there is no second field: `clip` is the first
  // entry of the list and the colour uses it too. The case that tells them
  // apart is `background-color-clip.html`, whose `background-image: none, none`
  // makes two layers and whose `background-clip: border-box, content-box,
  // border-box` therefore has its third entry *discarded* -- the bottom-most
  // layer is the second, not the last written. Answering "the last entry" would
  // be wrong there in a way that looks right, so it is not answered at all
  // until layers exist. See `image` above for the same decision.

  friend bool operator==(const BackgroundLayer&, const BackgroundLayer&) = default;
};

std::optional<BackgroundRepeat> ParseBackgroundRepeat(std::string_view value);

// One component of `background-position`, as the length it resolves to. The
// keywords are per-axis -- `right` is 100% horizontally and is not a value at
// all vertically -- which is why `horizontal` is a parameter and why
// `background-position-x` and `-y` are two properties rather than one indexed
// one. Declared here rather than copied into `BackgroundDeclarations.cpp`: two
// copies of a keyword table is two chances for `center` to mean two things.
std::optional<Length> ParseBackgroundPosition(std::string_view word, bool horizontal,
                                              const MediaContext& context, float root_font_size);

// The word-level half of the `background` shorthand: everything in it that is
// not the `url()` the caller already took. Repeat, colour, the one or two boxes,
// and the position.
//
// **The position is the half this used to drop**, and dropping it was invisible
// because a second bug cancelled it exactly: `background: url(x) -5px` on an
// element with a 5px border rendered correctly while the shorthand ignored the
// `-5px` *and* the painter measured from the border box instead of the padding
// box. Eight of `css/CSS2/margin-padding-clear/`'s tests passed on that pair and
// failed the moment either one was fixed alone.
bool ApplyBackgroundShorthandWords(std::string_view value, ComputedStyle& style,
                                   const MediaContext& context);

// The `background-*` longhands that are their own grammar rather than a piece
// of the shorthand: `background-origin`, `background-clip`, and the
// `background-position-x`/`-y` pair.
//
// Its own translation unit for the reason `ApplySvgDeclaration` is one --
// `Declarations.cpp` is at its module's `max_tu_lines` -- and its own function
// because these four are the only background properties that decide a *box*
// rather than a length, which is the seam the painter reads.
bool ApplyBackgroundDeclaration(std::string_view property, std::string_view value,
                                const ComputedStyle& parent, ComputedStyle& style,
                                const MediaContext& context);

}  // namespace microbrowser::css
