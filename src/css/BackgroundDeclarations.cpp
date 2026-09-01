#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "css/ComputedStyle.h"
#include "gfx/Color.h"
#include "css/CssText.h"

// The four `background-*` longhands that are not a piece of the shorthand's own
// grammar (ADR 0014). Its own translation unit because `Declarations.cpp` is at
// `src/css`'s `max_tu_lines`, and because these four belong together: two name
// a box, and two are the halves of `background-position`.

namespace microbrowser::css {

namespace {

// `border-box | padding-box | content-box`.
//
// `text` is deliberately absent from `background-clip`: it clips the background
// to the *glyphs* of the element's text, which needs the shaped runs as a mask
// and a scratch surface to composite through. Answering `content-box` for it --
// or accepting it and painting the ordinary background -- would make
// `@supports (background-clip: text)` say yes to a page whose whole design then
// depends on it, which is ADR 0012's argument for an absence over a stub.
std::optional<BackgroundBox> ParseBackgroundBox(std::string_view value) {
  const std::string lowered = Lowered(Trim(value));
  if (lowered == "border-box") {
    return BackgroundBox::BorderBox;
  }
  if (lowered == "padding-box") {
    return BackgroundBox::PaddingBox;
  }
  if (lowered == "content-box") {
    return BackgroundBox::ContentBox;
  }
  return std::nullopt;
}


}  // namespace

std::optional<BackgroundRepeat> ParseBackgroundRepeat(std::string_view word) {
  const std::string lowered = Lowered(Trim(word));
  if (lowered == "repeat") {
    return BackgroundRepeat::Repeat;
  }
  if (lowered == "repeat-x") {
    return BackgroundRepeat::RepeatX;
  }
  if (lowered == "repeat-y") {
    return BackgroundRepeat::RepeatY;
  }
  if (lowered == "no-repeat") {
    return BackgroundRepeat::NoRepeat;
  }
  return std::nullopt;
}

// The keywords are percentages of the space the image does *not* fill, which is
// what makes `center` centre rather than offset by half the box. One definition
// for the shorthand and both longhands.
std::optional<Length> ParseBackgroundPosition(std::string_view word, bool horizontal,
                                              const MediaContext& context, float root_font_size) {
  const std::string lowered = Lowered(Trim(word));
  if (lowered == "center") {
    return Length{50.0f, Length::Unit::Percent};
  }
  if (lowered == (horizontal ? "left" : "top")) {
    return Length::Pixels(0.0f);
  }
  if (lowered == (horizontal ? "right" : "bottom")) {
    return Length{100.0f, Length::Unit::Percent};
  }
  return ParseLength(word, context, root_font_size);
}

bool ApplyBackgroundDeclaration(std::string_view property, std::string_view value,
                                const ComputedStyle& parent, ComputedStyle& style,
                                const MediaContext& context) {
  (void)parent;
  if (property == "background-origin" || property == "background-clip") {
    // **A layer list, and every entry has to parse even though one is used.** A
    // list is one value: `background-clip: content-box, nonsense` is invalid in
    // full rather than a content-box clip, and a loop that stopped at the first
    // entry would accept it. The entry used is the first, because this renderer
    // paints one layer -- see `BackgroundLayer::image`.
    std::optional<BackgroundBox> first;
    for (const std::string_view layer : SplitTopLevel(value, ',')) {
      const std::optional<BackgroundBox> box = ParseBackgroundBox(layer);
      if (!box.has_value()) {
        return false;
      }
      if (!first.has_value()) {
        first = box;
      }
    }
    if (!first.has_value()) {
      return false;
    }
    (property == "background-origin" ? style.background.origin : style.background.clip) = *first;
    return true;
  }
  // **The longhands, and they are not simply "the first word of
  // `background-position`".** `background-position-x: right` is 100% and
  // `background-position-y: right` is not a value at all, because the keywords
  // are per-axis -- which is exactly what makes these two properties rather
  // than one with an index. The `x-start`/`x-end` (and `y-*`) writing-mode
  // keywords are absent for the same reason `background-clip: text` is: this
  // renderer has no writing mode to resolve them against, and a value that
  // resolved to `left` regardless would be wrong in Arabic and silently right
  // everywhere it was tested.
  const bool horizontal = property == "background-position-x";
  if (horizontal || property == "background-position-y") {
    const std::vector<std::string_view> words = SplitWords(value);
    // The two-word form is an edge offset: `right 10px` is 10px from the right
    // edge, which is not the same length as `10px` and not expressible in one.
    // Refused rather than half-read, because reading only `right` would put the
    // image at the far edge and silently drop the offset a page asked for.
    if (words.size() != 1) {
      return false;
    }
    const std::optional<Length> length =
        ParseBackgroundPosition(words.front(), horizontal, context, style.root_font_size);
    if (!length.has_value()) {
      return false;
    }
    (horizontal ? style.background.position_x : style.background.position_y) = *length;
    return true;
  }
  return false;
}

bool ApplyBackgroundShorthandWords(std::string_view value, ComputedStyle& style,
                                   const MediaContext& context) {
  bool understood = false;
  // The position's components, in the order they were written. Collected rather
  // than applied as they are found, because a position is one to two words and
  // which axis a lone `center` belongs to is not decidable until the list ends.
  std::vector<std::string_view> position;
  // `background: url(x) content-box` sets *both* boxes; a second box value sets
  // only the clip. That is the shorthand's own rule and it is why the count
  // matters rather than the position in the list.
  int boxes = 0;
  for (const std::string_view word : SplitWords(value)) {
    if (const std::optional<BackgroundRepeat> repeat = ParseBackgroundRepeat(word)) {
      style.background.repeat = *repeat;
      understood = true;
      continue;
    }
    if (const std::optional<BackgroundBox> box = ParseBackgroundBox(word)) {
      if (boxes == 0) {
        style.background.origin = *box;
        style.background.clip = *box;
      } else if (boxes == 1) {
        style.background.clip = *box;
      }
      ++boxes;
      understood = true;
      continue;
    }
    if (const std::optional<gfx::Color> color = ParseColor(word)) {
      style.background_color = *color;
      understood = true;
      continue;
    }
    // A `url()` was taken by the caller; anything else that parses as a
    // position component is one. Checked against *both* axes, because a bare
    // `left` is horizontal and a bare `top` vertical and neither is known to be
    // first.
    if (ParseBackgroundPosition(word, true, context, style.root_font_size).has_value() ||
        ParseBackgroundPosition(word, false, context, style.root_font_size).has_value()) {
      position.push_back(word);
    }
  }
  // One component sets the horizontal position and centres the vertical, which
  // is what CSS says and is the difference between an icon on the left edge and
  // one halfway down it. More than two is not a position this renderer reads;
  // leaving the pair alone is better than assigning half of one.
  if (!position.empty() && position.size() <= 2) {
    const std::optional<Length> x =
        ParseBackgroundPosition(position.front(), true, context, style.root_font_size);
    const std::optional<Length> y =
        position.size() == 2
            ? ParseBackgroundPosition(position[1], false, context, style.root_font_size)
            : std::optional<Length>(Length{50.0f, Length::Unit::Percent});
    if (x.has_value() && y.has_value()) {
      style.background.position_x = *x;
      style.background.position_y = *y;
      understood = true;
    }
  }
  return understood;
}

}  // namespace microbrowser::css
