#include "layout/ReplacedBoxes.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "css/ComputedStyle.h"
#include "dom/Node.h"
#include "gfx/Image.h"
#include "html/FormControl.h"
#include "util/Parse.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::layout {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

}  // namespace

std::string CollapseWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_space = false;
  for (const char c : text) {
    if (IsSpace(c)) {
      in_space = true;
      continue;
    }
    if (in_space) {
      out.push_back(' ');
    }
    in_space = false;
    out.push_back(c);
  }
  if (in_space) {
    out.push_back(' ');
  }
  return out;
}

namespace {

// Whether this element draws the user agent's own controls. ADR 0028 §1 puts them in ADR 0018's
// category: boxes the user agent creates inside a page rather than widgets `src/ui` owns.
bool DrawsDefaultControls(const dom::Element& element) {
  return (element.TagName() == "video" || element.TagName() == "audio") &&
         element.GetAttribute("controls") != nullptr;
}

float ReplacedIntrinsic(const Box& box, bool horizontal) {
  const css::ComputedStyle& style = box.Style();
  const css::Length& declared = horizontal ? style.width : style.height;
  if (!declared.IsAuto() && !declared.IsPercent()) {
    return std::max(0.0f, declared.Resolve(style.font_size, 0.0f));
  }
  if (box.Origin() != nullptr) {
    // The presentational attribute, which is where most of the web still puts
    // an image's size and which the cascade does not see.
    const std::string* attribute = box.Origin()->GetAttribute(horizontal ? "width" : "height");
    if (attribute != nullptr) {
      if (const std::optional<double> value = util::ParseDouble(*attribute)) {
        if (*value >= 0.0 && *value < 1e6) {
          return static_cast<float>(*value);
        }
      }
    }
  }
  // An aspect ratio with the other axis known, which is what reserves the box
  // for an image before the image has arrived. Checked before the image's own
  // size, because a page that states a ratio means it to win over one.
  if (style.aspect_ratio > 0.0f) {
    const css::Length& other = horizontal ? style.height : style.width;
    if (!other.IsAuto() && !other.IsPercent()) {
      const float extent = std::max(0.0f, other.Resolve(style.font_size, 0.0f));
      return horizontal ? extent * style.aspect_ratio : extent / style.aspect_ratio;
    }
  }
  if (box.Image() != nullptr && box.Image()->IsValid()) {
    return static_cast<float>(horizontal ? box.Image()->Width() : box.Image()->Height());
  }
  if (box.Origin() != nullptr &&
      (box.Origin()->TagName() == "video" || box.Origin()->TagName() == "audio")) {
    if (box.Origin()->TagName() == "audio") {
      // An `<audio>` is exactly its controls: 300x54 with them and *nothing* without, which is
      // what makes one used as a sound effect take no space.
      if (!HasDefaultControls(*box.Origin())) {
        return 0.0f;
      }
      return horizontal ? 300.0f : 54.0f;
    }
    // 300x150, the specification's default video size. A page laying out around a video before
    // it loads is laying out around this number, so a wrong one moves the page when it appears.
    return horizontal ? 300.0f : 150.0f;
  }
  if (box.Origin() != nullptr &&
      (box.Origin()->TagName() == "input" || box.Origin()->TagName() == "textarea" ||
       box.Origin()->TagName() == "select")) {
    if (!horizontal) {
      if (box.Origin()->TagName() == "textarea") {
        const std::string* rows = box.Origin()->GetAttribute("rows");
        if (rows != nullptr) {
          if (const std::optional<double> parsed = util::ParseDouble(*rows)) {
            if (*parsed > 0.0 && *parsed < 1000.0) {
              return static_cast<float>(*parsed) * style.font_size * 1.2f + 6.0f;
            }
          }
        }
      }
      return style.font_size * 1.2f + 6.0f;
    }
    const std::string* size =
        box.Origin()->GetAttribute(box.Origin()->TagName() == "textarea" ? "cols" : "size");
    if (size != nullptr) {
      if (const std::optional<double> parsed = util::ParseDouble(*size)) {
        if (*parsed > 0.0 && *parsed < 1000.0) {
          return static_cast<float>(*parsed) * style.font_size * 0.6f + 12.0f;
        }
      }
    }
    if (!box.Text().empty()) {
      return static_cast<float>(box.Text().size()) * style.font_size * 0.6f + 18.0f;
    }
    return style.font_size * 20.0f * 0.6f + 12.0f;
  }
  // Inline `<svg>` is a replaced element (CSS 2.1 / SVG). Without an intrinsic
  // size it collapses to 0×0 and youtube's masthead logo disappears — width and
  // height from CSS only apply to replaced inlines.
  if (box.Origin() != nullptr && box.Origin()->TagName() == "svg") {
    if (const std::string* view_box = box.Origin()->GetAttribute("viewbox")) {
      // `min-x min-y width height`, whitespace-separated.
      float numbers[4] = {};
      std::size_t count = 0;
      std::size_t i = 0;
      while (i < view_box->size() && count < 4) {
        while (i < view_box->size() && IsSpace((*view_box)[i])) {
          ++i;
        }
        if (i >= view_box->size()) {
          break;
        }
        const std::size_t begin = i;
        while (i < view_box->size() && !IsSpace((*view_box)[i]) && (*view_box)[i] != ',') {
          ++i;
        }
        if (const std::optional<double> parsed =
                util::ParseDouble(std::string_view(view_box->data() + begin, i - begin))) {
          numbers[count++] = static_cast<float>(*parsed);
        } else {
          break;
        }
        if (i < view_box->size() && (*view_box)[i] == ',') {
          ++i;
        }
      }
      if (count == 4 && numbers[2] > 0.0f && numbers[3] > 0.0f) {
        return horizontal ? numbers[2] : numbers[3];
      }
    }
  }
  return 0.0f;
}


}  // namespace

bool HasDefaultControls(const dom::Element& element) { return DrawsDefaultControls(element); }

bool IsReplacedElement(const dom::Element& element) {
  // `<video>` and `<audio>` too (ADR 0028 §1): their content comes from outside CSS and their
  // children are fallback the element replaces. Without this a `<video>` lays out its `<source>`
  // children and its "your browser does not support" paragraph as page content.
  // `<canvas>` too (ADR 0029 §2): its content is a bitmap the page drew, and its children are fallback
  // it replaces -- without this, the "your browser does not support canvas" paragraph inside one is
  // laid out as page content.
  // Inline `<svg>` too: same replaced model as `<img>`, with intrinsic size from
  // width/height attributes or `viewBox`. Left as a normal box it ignores CSS
  // width/height (non-replaced inline) and paints nothing of its own.
  //
  // `<button>` is deliberately **not** here. It is a normal block container
  // whose children are real boxes -- that is what makes `<button><span
  // class=icon></span><span class=label>Accept all</span></button>` measure its
  // label rather than a character count. Treating it as replaced gave every
  // button on youtube a width from `chars * 0.6 * font-size` and no boxes at all
  // under it, so a flex item button collapsed to its flex base and clipped its
  // own label. `CentersContentVertically` is the one thing the replaced path was
  // getting right, and it is kept.
  return element.TagName() == "img" || element.TagName() == "input" ||
         element.TagName() == "textarea" || element.TagName() == "select" ||
         element.TagName() == "video" || element.TagName() == "audio" ||
         element.TagName() == "canvas" || element.TagName() == "svg";
}

bool CentersContentVertically(const dom::Element& element) {
  // A `<button>`'s content sits in the middle of its content box, not at the
  // top: `<button style="height:40px">Go</button>` centres its label. Blink and
  // WebKit both do it by wrapping the children in an anonymous box and centring
  // that; nothing in CSS expresses the rule, so it lives beside the layout that
  // needs it rather than in the user-agent stylesheet.
  return element.TagName() == "button";
}

float ReplacedWidth(const Box& box) { return ReplacedIntrinsic(box, true); }

float ReplacedHeight(const Box& box) { return ReplacedIntrinsic(box, false); }

ReplacedUsedSize ResolveReplacedSize(const Box& box, float containing_block_width,
                                     std::optional<float> containing_block_height) {
  const css::ComputedStyle& style = box.Style();
  ReplacedUsedSize used;
  bool resolved_percent = false;

  if (style.width.IsPercent()) {
    used.width = std::max(0.0f, style.width.Used(containing_block_width, style.font_size));
    resolved_percent = true;
  } else if (!style.width.IsAuto()) {
    used.width = std::max(0.0f, style.width.Resolve(style.font_size, 0.0f));
  } else {
    used.width = ReplacedWidth(box);
  }

  if (style.height.IsPercent()) {
    if (containing_block_height.has_value()) {
      used.height =
          std::max(0.0f, style.height.Used(*containing_block_height, style.font_size));
      resolved_percent = true;
    } else {
      // Indefinite CB height: percentage height computes as auto (§10.5).
      used.height = ReplacedHeight(box);
    }
  } else if (!style.height.IsAuto()) {
    used.height = std::max(0.0f, style.height.Resolve(style.font_size, 0.0f));
  } else {
    used.height = ReplacedHeight(box);
  }

  // One axis auto, the other definite, and an aspect ratio: fill the auto axis
  // from the ratio rather than from an intrinsic that would disagree with the
  // percentage we just applied (FillParent + later-decoded bitmap).
  if (style.aspect_ratio > 0.0f) {
    const bool height_definite =
        !style.height.IsAuto() &&
        (!style.height.IsPercent() || containing_block_height.has_value());
    const bool width_definite = !style.width.IsAuto();
    if (style.width.IsAuto() && height_definite && used.height > 0.0f) {
      used.width = used.height * style.aspect_ratio;
    } else if (style.height.IsAuto() && width_definite && used.width > 0.0f) {
      used.height = used.width / style.aspect_ratio;
    }
  }

  if (resolved_percent) {
    util::AddPerformanceCounter(util::PerfCounterId::LayoutReplacedPercentResolved);
  }
  return used;
}

std::string FormControlText(const dom::Element& element) {
  if (const std::optional<std::string> selected = html::SelectedOptionText(element)) {
    return *selected;
  }
  if (html::IsTextareaElement(element)) {
    const std::string* value = element.GetAttribute("value");
    const std::string current = value != nullptr ? *value : element.TextContent();
    if (!current.empty()) {
      return current;
    }
    if (const std::string* placeholder = element.GetAttribute("placeholder")) {
      return *placeholder;
    }
    return {};
  }
  if (html::IsCheckboxInput(element) || html::IsRadioInput(element)) {
    return {};
  }
  if (const std::string* value = element.GetAttribute("value"); value != nullptr && !value->empty()) {
    if (html::IsPasswordInput(element)) {
      return {};
    }
    return *value;
  }
  if (html::IsTextInputType(element)) {
    if (const std::string* placeholder = element.GetAttribute("placeholder")) {
      return *placeholder;
    }
    return {};
  }
  if (html::IsSubmitControl(element)) {
    return "Submit";
  }
  if (html::IsResetControl(element)) {
    return "Reset";
  }
  return {};
}

}  // namespace microbrowser::layout
