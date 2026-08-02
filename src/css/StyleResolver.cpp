#include "css/StyleResolver.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "util/Parse.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), ToLower);
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

bool AcceptsBgColorAttribute(std::string_view tag_name) {
  return tag_name == "body" || tag_name == "table" || tag_name == "tr" || tag_name == "td" ||
         tag_name == "th";
}

std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && text[start] == ' ') {
      ++start;
    }
    std::size_t end = start;
    while (end < text.size() && text[end] != ' ') {
      ++end;
    }
    if (end > start) {
      words.push_back(text.substr(start, end - start));
    }
    start = end;
  }
  return words;
}

struct NamedColor {
  std::string_view name;
  std::uint32_t argb;
};

// The colours a real page actually uses by name. The full CSS list is 148
// entries of generated data; growing this is a data change.
constexpr std::array<NamedColor, 22> kNamedColors = {{
    {"black", 0xFF000000},   {"white", 0xFFFFFFFF},   {"red", 0xFFFF0000},
    {"green", 0xFF008000},   {"blue", 0xFF0000FF},    {"yellow", 0xFFFFFF00},
    {"cyan", 0xFF00FFFF},    {"magenta", 0xFFFF00FF}, {"gray", 0xFF808080},
    {"grey", 0xFF808080},    {"silver", 0xFFC0C0C0},  {"maroon", 0xFF800000},
    {"olive", 0xFF808000},   {"lime", 0xFF00FF00},    {"aqua", 0xFF00FFFF},
    {"teal", 0xFF008080},    {"navy", 0xFF000080},    {"fuchsia", 0xFFFF00FF},
    {"purple", 0xFF800080},  {"orange", 0xFFFFA500},  {"transparent", 0x00000000},
    {"currentcolor", 0xFF000000},
}};

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::optional<std::uint8_t> ParseRgbChannel(std::string_view text) {
  if (text.ends_with('%')) {
    const std::optional<double> percent = util::ParseDouble(text.substr(0, text.size() - 1));
    if (!percent.has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::clamp(*percent, 0.0, 100.0) * 255.0 / 100.0 + 0.5);
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(std::clamp(*value, 0.0, 255.0) + 0.5);
}

std::optional<std::uint8_t> ParseAlphaChannel(std::string_view text) {
  if (text.ends_with('%')) {
    const std::optional<double> percent = util::ParseDouble(text.substr(0, text.size() - 1));
    if (!percent.has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::clamp(*percent, 0.0, 100.0) * 255.0 / 100.0 + 0.5);
  }
  const std::optional<double> value = util::ParseDouble(text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(std::clamp(*value, 0.0, 1.0) * 255.0 + 0.5);
}

}  // namespace

float Length::Resolve(float font_size, float fallback) const {
  switch (unit) {
    case Unit::Pixels:
      return value;
    case Unit::Em:
      return value * font_size;
    case Unit::Rem:
      // The root font size is 16 until a root style exists to ask. Carrying the
      // root style through every Resolve call would put a parameter on a
      // function that is called per edge per element per frame.
      return value * 16.0f;
    case Unit::Percent:
    case Unit::Auto:
      return fallback;
  }
  return fallback;
}

std::optional<gfx::Color> ParseColor(std::string_view text) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered.empty()) {
    return std::nullopt;
  }

  if (lowered.front() == '#') {
    const std::string_view digits(lowered.data() + 1, lowered.size() - 1);
    const auto nibble = [&digits](std::size_t index) {
      return static_cast<std::uint32_t>(HexValue(digits[index]));
    };
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return HexValue(c) >= 0; })) {
      return std::nullopt;
    }
    if (digits.size() == 3) {
      // `#abc` is `#aabbcc` — each digit doubled, not shifted, so `#fff` is
      // exactly white rather than 0xF0F0F0.
      return gfx::Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 17),
                             static_cast<std::uint8_t>(nibble(1) * 17),
                             static_cast<std::uint8_t>(nibble(2) * 17));
    }
    if (digits.size() == 6) {
      return gfx::Color::Rgb(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
                             static_cast<std::uint8_t>(nibble(2) * 16 + nibble(3)),
                             static_cast<std::uint8_t>(nibble(4) * 16 + nibble(5)));
    }
    if (digits.size() == 8) {
      return gfx::Color::Rgba(static_cast<std::uint8_t>(nibble(0) * 16 + nibble(1)),
                              static_cast<std::uint8_t>(nibble(2) * 16 + nibble(3)),
                              static_cast<std::uint8_t>(nibble(4) * 16 + nibble(5)),
                              static_cast<std::uint8_t>(nibble(6) * 16 + nibble(7)));
    }
    return std::nullopt;
  }

  if (lowered.rfind("rgb(", 0) == 0 || lowered.rfind("rgba(", 0) == 0) {
    const std::size_t open = lowered.find('(');
    const std::size_t close = lowered.rfind(')');
    if (close == std::string::npos || close < open) {
      return std::nullopt;
    }
    std::string body = lowered.substr(open + 1, close - open - 1);
    std::replace(body.begin(), body.end(), ',', ' ');
    const std::vector<std::string_view> parts = SplitWords(body);
    if (parts.size() != 3 && parts.size() != 4) {
      return std::nullopt;
    }
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t i = 0; i < 3; ++i) {
      const std::optional<std::uint8_t> channel = ParseRgbChannel(parts[i]);
      if (!channel.has_value()) {
        return std::nullopt;
      }
      channels[i] = *channel;
    }
    std::uint8_t alpha = 255;
    if (parts.size() == 4) {
      const std::optional<std::uint8_t> parsed_alpha = ParseAlphaChannel(parts[3]);
      if (!parsed_alpha.has_value()) {
        return std::nullopt;
      }
      alpha = *parsed_alpha;
    }
    return gfx::Color::Rgba(channels[0], channels[1], channels[2], alpha);
  }

  for (const NamedColor& named : kNamedColors) {
    if (named.name == lowered) {
      return gfx::Color{named.argb};
    }
  }
  return std::nullopt;
}

std::optional<Length> ParseLength(std::string_view text) {
  const std::string lowered = Lowered(Trim(text));
  if (lowered.empty()) {
    return std::nullopt;
  }
  if (lowered == "auto") {
    return Length::Auto();
  }

  std::size_t at = 0;
  double sign = 1.0;
  if (at < lowered.size() && (lowered[at] == '+' || lowered[at] == '-')) {
    sign = lowered[at] == '-' ? -1.0 : 1.0;
    ++at;
  }
  double value = 0.0;
  bool any_digit = false;
  while (at < lowered.size() && lowered[at] >= '0' && lowered[at] <= '9') {
    value = value * 10.0 + (lowered[at++] - '0');
    any_digit = true;
  }
  if (at < lowered.size() && lowered[at] == '.') {
    ++at;
    double step = 0.1;
    while (at < lowered.size() && lowered[at] >= '0' && lowered[at] <= '9') {
      value += (lowered[at++] - '0') * step;
      step *= 0.1;
      any_digit = true;
    }
  }
  if (!any_digit) {
    return std::nullopt;
  }
  value *= sign;

  const std::string_view unit(lowered.data() + at, lowered.size() - at);
  if (unit.empty()) {
    // A unitless number is a length only when it is zero. `width: 5` is not
    // five pixels, it is an invalid declaration, and treating it as pixels is
    // how a page renders differently here than everywhere else.
    return value == 0.0 ? std::optional<Length>(Length::Pixels(0.0f)) : std::nullopt;
  }
  if (unit == "px") {
    return Length::Pixels(static_cast<float>(value));
  }
  if (unit == "%") {
    return Length{static_cast<float>(value), Length::Unit::Percent};
  }
  if (unit == "em") {
    return Length{static_cast<float>(value), Length::Unit::Em};
  }
  if (unit == "rem") {
    return Length{static_cast<float>(value), Length::Unit::Rem};
  }
  if (unit == "pt") {
    // 1pt is 4/3 px, which is the one absolute unit conversion a browser
    // actually needs.
    return Length::Pixels(static_cast<float>(value * 4.0 / 3.0));
  }
  return std::nullopt;
}

namespace {

void ApplyEdges(std::string_view value, const ComputedStyle& style, Edges& edges) {
  const std::vector<std::string_view> parts = SplitWords(value);
  std::array<Length, 4> lengths;
  for (std::size_t i = 0; i < parts.size() && i < 4; ++i) {
    const auto length = ParseLength(parts[i]);
    if (!length.has_value()) {
      return;  // one bad component invalidates the whole shorthand
    }
    lengths[i] = *length;
  }
  (void)style;
  switch (parts.size()) {
    case 1:
      edges = Edges{lengths[0], lengths[0], lengths[0], lengths[0]};
      return;
    case 2:
      edges = Edges{lengths[0], lengths[1], lengths[0], lengths[1]};
      return;
    case 3:
      edges = Edges{lengths[0], lengths[1], lengths[2], lengths[1]};
      return;
    case 4:
      edges = Edges{lengths[0], lengths[1], lengths[2], lengths[3]};
      return;
    default:
      return;
  }
}

}  // namespace

void ApplyDeclaration(const Declaration& declaration, const ComputedStyle& parent,
                      ComputedStyle& style) {
  const std::string& property = declaration.property;
  const std::string value = Lowered(declaration.value);

  if (property == "display") {
    if (value == "block") {
      style.display = Display::Block;
    } else if (value == "inline") {
      style.display = Display::Inline;
    } else if (value == "inline-block") {
      style.display = Display::InlineBlock;
    } else if (value == "list-item") {
      style.display = Display::ListItem;
    } else if (value == "table") {
      style.display = Display::Table;
    } else if (value == "table-caption") {
      style.display = Display::TableCaption;
    } else if (value == "table-column-group") {
      style.display = Display::TableColumnGroup;
    } else if (value == "table-column") {
      style.display = Display::TableColumn;
    } else if (value == "table-header-group") {
      style.display = Display::TableHeaderGroup;
    } else if (value == "table-footer-group") {
      style.display = Display::TableFooterGroup;
    } else if (value == "table-row-group") {
      style.display = Display::TableRowGroup;
    } else if (value == "table-row") {
      style.display = Display::TableRow;
    } else if (value == "table-cell") {
      style.display = Display::TableCell;
    } else if (value == "none") {
      style.display = Display::None;
    }
    return;
  }
  if (property == "color") {
    if (const auto color = ParseColor(declaration.value)) {
      style.color = *color;
    }
    return;
  }
  if (property == "background-color" || property == "background") {
    if (const auto color = ParseColor(declaration.value)) {
      style.background_color = *color;
    }
    return;
  }
  if (property == "font-size") {
    // Resolved to absolute pixels during the cascade, because `em` on every
    // other property is relative to it — and `font-size: 2em` is relative to
    // the *parent's* size, not its own.
    if (value == "smaller") {
      style.font_size = parent.font_size * 0.8f;
      return;
    }
    if (value == "larger") {
      style.font_size = parent.font_size * 1.25f;
      return;
    }
    if (const auto length = ParseLength(declaration.value)) {
      if (length->unit == Length::Unit::Percent) {
        style.font_size = parent.font_size * length->value / 100.0f;
      } else if (length->unit == Length::Unit::Em) {
        style.font_size = parent.font_size * length->value;
      } else if (!length->IsAuto()) {
        style.font_size = length->Resolve(parent.font_size);
      }
      style.font_size = std::clamp(style.font_size, 1.0f, 1000.0f);
    }
    return;
  }
  if (property == "font-weight") {
    if (value == "bold") {
      style.font_weight = 700.0f;
    } else if (value == "normal") {
      style.font_weight = 400.0f;
    } else if (const auto weight = util::ParseInt(value)) {
      style.font_weight = static_cast<float>(std::clamp(*weight, 1, 1000));
    }
    return;
  }
  if (property == "float") {
    if (value == "left") {
      style.css_float = Float::Left;
    } else if (value == "right") {
      style.css_float = Float::Right;
    } else if (value == "none") {
      style.css_float = Float::None;
    }
    return;
  }
  if (property == "clear") {
    if (value == "left") {
      style.clear = Clear::Left;
    } else if (value == "right") {
      style.clear = Clear::Right;
    } else if (value == "both") {
      style.clear = Clear::Both;
    } else if (value == "none") {
      style.clear = Clear::None;
    }
    return;
  }
  if (property == "font-style") {
    if (value == "italic" || value == "oblique") {
      style.font_style = FontStyle::Italic;
    } else if (value == "normal") {
      style.font_style = FontStyle::Normal;
    }
    return;
  }
  if (property == "font-family") {
    style.font_family = std::string(Trim(declaration.value));
    return;
  }
  if (property == "line-height") {
    if (value == "normal") {
      style.line_height = 0.0f;
      return;
    }
    if (const auto length = ParseLength(declaration.value)) {
      const float resolved = length->unit == Length::Unit::Percent
                                 ? style.font_size * length->value / 100.0f
                                 : length->Resolve(style.font_size, -1.0f);
      if (resolved >= 0.0f) {
        style.line_height = resolved;
      }
    } else if (const auto multiple = util::ParseDouble(value)) {
      if (*multiple >= 0.0) {
        style.line_height = static_cast<float>(*multiple) * style.font_size;
      }
    }
    return;
  }
  if (property == "text-align") {
    if (value == "center") {
      style.text_align = TextAlign::Center;
    } else if (value == "right") {
      style.text_align = TextAlign::Right;
    } else if (value == "justify") {
      style.text_align = TextAlign::Justify;
    } else if (value == "left") {
      style.text_align = TextAlign::Left;
    }
    return;
  }
  if (property == "white-space") {
    if (value == "pre") {
      style.white_space = WhiteSpace::Pre;
    } else if (value == "nowrap") {
      style.white_space = WhiteSpace::NoWrap;
    } else if (value == "pre-wrap") {
      style.white_space = WhiteSpace::PreWrap;
    } else if (value == "normal") {
      style.white_space = WhiteSpace::Normal;
    }
    return;
  }
  if (property == "margin") {
    ApplyEdges(declaration.value, style, style.margin);
    return;
  }
  if (property == "padding") {
    ApplyEdges(declaration.value, style, style.padding);
    return;
  }
  if (property == "width" || property == "height") {
    if (const auto length = ParseLength(declaration.value)) {
      (property == "width" ? style.width : style.height) = *length;
    }
    return;
  }
  if (property == "border-color") {
    if (const auto color = ParseColor(declaration.value)) {
      style.border_color = *color;
      style.has_border = true;
    }
    return;
  }
  if (property == "border-width") {
    ApplyEdges(declaration.value, style, style.border_width);
    style.has_border = true;
    return;
  }
  if (property == "border") {
    // `1px solid red` in any order. Each component is tried as a length then as
    // a colour; the style keyword is recognized and ignored, since only solid
    // is drawn.
    for (const std::string_view part : SplitWords(declaration.value)) {
      if (const auto length = ParseLength(part)) {
        style.border_width = Edges{*length, *length, *length, *length};
      } else if (const auto color = ParseColor(part)) {
        style.border_color = *color;
      }
    }
    style.has_border = true;
    return;
  }

  // Individual edge properties. Written as a loop rather than sixteen branches.
  static constexpr std::array<std::string_view, 4> kSides = {"top", "right", "bottom", "left"};
  for (std::size_t i = 0; i < kSides.size(); ++i) {
    const std::string margin_name = "margin-" + std::string(kSides[i]);
    const std::string padding_name = "padding-" + std::string(kSides[i]);
    if (property == margin_name || property == padding_name) {
      const auto length = ParseLength(declaration.value);
      if (!length.has_value()) {
        return;
      }
      Edges& edges = property == margin_name ? style.margin : style.padding;
      (&edges.top)[i] = *length;
      return;
    }
  }
  // Anything else is a property we do not implement. Ignored rather than
  // guessed at.
}

StyleResolver::StyleResolver() {
  AddStyleSheet(ParseStyleSheet(UserAgentStyleSheet()), Origin::UserAgent);
}

void StyleResolver::AddStyleSheet(const StyleSheet& sheet, Origin origin) {
  for (const StyleRule& rule : sheet.rules) {
    for (const Selector& selector : rule.selectors) {
      Entry entry;
      entry.selector = selector;
      entry.declarations = rule.declarations;
      entry.origin = origin;
      entry.specificity = selector.ComputeSpecificity();
      entry.order = next_order_++;
      rules_.push_back(std::move(entry));
    }
  }
}

ComputedStyle StyleResolver::StyleFor(const dom::Element& element,
                                      const ComputedStyle& parent) const {
  AddPerformanceCounter(PerfCounterId::CssStylesResolved);

  // Inherited properties start from the parent; everything else starts at its
  // initial value. Doing this by construction rather than by a per-property
  // `inherit` check is what makes the resolve one pass.
  ComputedStyle style;
  style.color = parent.color;
  style.font_size = parent.font_size;
  style.font_weight = parent.font_weight;
  style.font_style = parent.font_style;
  style.font_family = parent.font_family;
  style.line_height = parent.line_height;
  style.text_align = parent.text_align;
  style.white_space = parent.white_space;

  // The style attribute participates in the cascade rather than being applied
  // after it. Applied afterwards, it would beat an `!important` author rule,
  // which is backwards: importance is compared *before* specificity, and the
  // style attribute is author-origin with a specificity above any selector.
  std::vector<Declaration> inline_declarations;
  if (const std::string* inline_style = element.GetAttribute("style")) {
    inline_declarations = ParseDeclarationList(*inline_style);
  }
  std::vector<Declaration> presentational_declarations;
  if (AcceptsBgColorAttribute(element.TagName())) {
    if (const std::string* bgcolor = element.GetAttribute("bgcolor")) {
      presentational_declarations.push_back(
          Declaration{"background-color", *bgcolor, false});
    }
  }

  struct Candidate {
    const Declaration* declaration;
    Origin origin;
    Specificity specificity;
    std::size_t order;
  };

  std::vector<Candidate> ordered;
  for (const Entry& entry : rules_) {
    if (!entry.selector.Matches(element)) {
      continue;
    }
    for (const Declaration& declaration : entry.declarations) {
      ordered.push_back(Candidate{&declaration, entry.origin, entry.specificity, entry.order});
    }
  }
  for (const Declaration& declaration : presentational_declarations) {
    ordered.push_back(Candidate{&declaration, Origin::Author, Specificity{}, 0});
  }
  // Specificity above every selector, which is what "the style attribute wins
  // within its origin" means concretely.
  constexpr Specificity kInlineSpecificity{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  for (const Declaration& declaration : inline_declarations) {
    ordered.push_back(
        Candidate{&declaration, Origin::Inline, kInlineSpecificity, next_order_ + 1});
  }

  // Origin first, then importance — which *reverses* the origin order, so a
  // user-agent important rule beats an author one. Then specificity, then
  // document order.
  std::stable_sort(ordered.begin(), ordered.end(), [](const Candidate& a, const Candidate& b) {
    const int rank_a = (a.declaration->important ? 10 : 0) + static_cast<int>(a.origin);
    const int rank_b = (b.declaration->important ? 10 : 0) + static_cast<int>(b.origin);
    if (rank_a != rank_b) {
      return rank_a < rank_b;
    }
    if (!(a.specificity == b.specificity)) {
      return a.specificity < b.specificity;
    }
    return a.order < b.order;
  });

  for (const Candidate& candidate : ordered) {
    ApplyDeclaration(*candidate.declaration, parent, style);
  }
  return style;
}

std::string_view UserAgentStyleSheet() {
  // Without this, `<div>` is inline and every document is one long line. The
  // values are the ones the HTML specification's rendering section gives.
  return R"CSS(
html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, section, article,
header, footer, nav, aside, main, blockquote, pre, form, figure, hr {
  display: block;
}
li { display: list-item }
input, button, textarea, select {
  display: inline-block; background-color: white; border: 1px solid gray
}
table { display: table }
caption { display: table-caption }
colgroup { display: table-column-group }
col { display: table-column }
thead { display: table-header-group }
tbody { display: table-row-group }
tfoot { display: table-footer-group }
tr { display: table-row }
td, th { display: table-cell }
head, style, script, title, meta, link { display: none }
body { margin: 8px }
p { margin: 1em 0 }
h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0 }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0 }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0 }
h4 { font-weight: bold; margin: 1.33em 0 }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0 }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0 }
b, strong { font-weight: bold }
i, em { font-style: italic }
a { color: #0000EE }
ul, ol { margin: 1em 0; padding-left: 40px }
blockquote { margin: 1em 40px }
pre { white-space: pre; margin: 1em 0 }
hr { margin: 0.5em 0; border-width: 1px; border-color: gray }
)CSS";
}

}  // namespace microbrowser::css
