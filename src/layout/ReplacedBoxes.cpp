#include "layout/ReplacedBoxes.h"

#include <algorithm>
#include <optional>
#include <string>

#include "css/ComputedStyle.h"
#include "dom/Node.h"
#include "gfx/Image.h"
#include "html/FormControl.h"
#include "util/Parse.h"
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
  if (box.Origin() != nullptr && (box.Origin()->TagName() == "input" ||
                                  box.Origin()->TagName() == "button" ||
                                  box.Origin()->TagName() == "textarea" ||
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
  return 0.0f;
}


}  // namespace

bool HasDefaultControls(const dom::Element& element) { return DrawsDefaultControls(element); }

bool IsReplacedElement(const dom::Element& element) {
  // `<video>` and `<audio>` too (ADR 0028 §1): their content comes from outside CSS and their
  // children are fallback the element replaces. Without this a `<video>` lays out its `<source>`
  // children and its "your browser does not support" paragraph as page content.
  return element.TagName() == "img" || element.TagName() == "input" ||
         element.TagName() == "button" || element.TagName() == "textarea" ||
         element.TagName() == "select" || element.TagName() == "video" ||
         element.TagName() == "audio";
}

float ReplacedWidth(const Box& box) { return ReplacedIntrinsic(box, true); }

float ReplacedHeight(const Box& box) { return ReplacedIntrinsic(box, false); }

std::string FormControlText(const dom::Element& element) {
  if (const std::optional<std::string> selected = html::SelectedOptionText(element)) {
    return *selected;
  }
  if (element.TagName() == "button") {
    return CollapseWhitespace(element.TextContent());
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
