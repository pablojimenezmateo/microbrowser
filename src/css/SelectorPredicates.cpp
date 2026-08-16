#include "css/SelectorPredicates.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Namespaces.h"
#include "text/Bidi.h"
#include "util/StringUtil.h"

namespace microbrowser::css {

namespace {

// The attributes HTML matches case-insensitively when nothing says otherwise.
//
// This is a *list in the specification* rather than a rule that can be derived,
// which is why it is written out: an author who writes `[type=CHECKBOX]` in an
// HTML document is matching a checkbox, and one who writes `[id=Foo]` is not
// matching `id="foo"`. The `s` flag is what turns the first of those off, and
// the reason `AttributeCase::Default` is a third state rather than a synonym
// for `Sensitive`.
bool HtmlMatchesCaselessly(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "accept",   "accept-charset", "align",     "alink",    "axis",       "bgcolor",
      "charset",  "checked",        "clear",     "codetype", "color",      "compact",
      "declare",  "defer",          "dir",       "direction","disabled",   "enctype",
      "face",     "frame",          "hreflang",  "http-equiv","lang",      "language",
      "link",     "media",          "method",    "multiple", "nohref",     "noresize",
      "noshade",  "nowrap",         "readonly",  "rel",      "rev",        "rules",
      "scope",    "scrolling",      "selected",  "shape",    "target",     "text",
      "type",     "valign",         "valuetype", "vlink",
  };
  for (const std::string_view candidate : kNames) {
    if (candidate == name) {
      return true;
    }
  }
  return false;
}

// Whether `element` is an HTML element, which is what decides whether the list
// above applies at all -- an `<a type="FOO">` inside an `<svg>` is matched case
// sensitively and an `<a type="FOO">` outside one is not.
//
// It should be one namespace comparison, and for an element script created it
// is: `createElementNS` records what it was given. For an element the *parser*
// created it cannot be, because `src/html` has no foreign-content insertion
// modes yet -- everything it builds is in the HTML namespace, including the
// contents of `<svg>`. So the ancestor walk is standing in for a namespace this
// tree cannot yet report, and it is here rather than in `src/html` because
// giving the parser real foreign namespaces is its own change with its own
// blast radius. **Delete this walk when it has them**; the namespace test above
// it is the whole answer.
//
// Only reached for an attribute on the legacy list, so an ordinary `[href]` or
// `[data-x]` never pays for it.
bool IsHtmlElement(const dom::Element& element) {
  if (!element.Namespace().IsHtml()) {
    return false;
  }
  for (const dom::Node* at = element.Parent(); at != nullptr; at = at->Parent()) {
    if (!at->IsElement()) {
      break;
    }
    const std::string_view tag = static_cast<const dom::Element&>(*at).TagName();
    if (tag == "svg" || tag == "math") {
      return false;
    }
  }
  return true;
}

bool ValueMatches(const SelectorPart& part, std::string_view value, bool caseless) {
  // The one ASCII case folder rather than a copy: `util::EqualsAsciiCaseInsensitive`
  // allocates nothing, where `Lowered(a) == Lowered(b)` would allocate twice per
  // comparison on a path that runs once per attribute per rule per element.
  const auto equals = [&](std::string_view a, std::string_view b) {
    return caseless ? util::EqualsAsciiCaseInsensitive(a, b) : a == b;
  };
  switch (part.match) {
    case SelectorPart::AttributeMatch::Exists:
      return true;
    case SelectorPart::AttributeMatch::Equals:
      return equals(value, part.value);
    case SelectorPart::AttributeMatch::Includes: {
      if (part.value.empty()) {
        return false;
      }
      std::size_t start = 0;
      while (start < value.size()) {
        while (start < value.size() && util::IsHtmlWhitespace(value[start])) {
          ++start;
        }
        std::size_t end = start;
        while (end < value.size() && !util::IsHtmlWhitespace(value[end])) {
          ++end;
        }
        if (end > start && equals(value.substr(start, end - start), part.value)) {
          return true;
        }
        start = end;
      }
      return false;
    }
    case SelectorPart::AttributeMatch::DashMatch:
      return equals(value, part.value) ||
             (value.size() > part.value.size() &&
              equals(value.substr(0, part.value.size()), part.value) &&
              value[part.value.size()] == '-');
    case SelectorPart::AttributeMatch::Prefix:
      return !part.value.empty() && value.size() >= part.value.size() &&
             equals(value.substr(0, part.value.size()), part.value);
    case SelectorPart::AttributeMatch::Suffix:
      return !part.value.empty() && value.size() >= part.value.size() &&
             equals(value.substr(value.size() - part.value.size()), part.value);
    case SelectorPart::AttributeMatch::Substring: {
      if (part.value.empty() || value.size() < part.value.size()) {
        return false;
      }
      for (std::size_t at = 0; at + part.value.size() <= value.size(); ++at) {
        if (equals(value.substr(at, part.value.size()), part.value)) {
          return true;
        }
      }
      return false;
    }
  }
  return false;
}

}  // namespace

bool TypeSelectorMatches(const SelectorPart& part, const dom::Element& element) {
  switch (part.name_space) {
    case SelectorPart::NamespaceMatch::Default:
      // No default namespace is ever declared, so an unprefixed type selector
      // matches in any namespace -- which is what the qualified name compares.
      return element.TagName() == part.name;
    case SelectorPart::NamespaceMatch::Any:
      return element.LocalName() == part.name;
    case SelectorPart::NamespaceMatch::None:
      // `|div` wants an element in *no* namespace. Every element in an HTML
      // document is in the HTML namespace, so this matches nothing there --
      // which is correct and is why it is spelled out rather than folded into
      // the case above.
      return element.Namespace().IsNone() && element.LocalName() == part.name;
    case SelectorPart::NamespaceMatch::Named:
      // The prefix is a name, not a URI, until `@namespace` reaches the cascade.
      return false;
  }
  return false;
}

bool AttributeSelectorMatches(const SelectorPart& part, const dom::Element& element) {
  const bool caseless =
      part.attribute_case == SelectorPart::AttributeCase::Insensitive ||
      (part.attribute_case == SelectorPart::AttributeCase::Default &&
       HtmlMatchesCaselessly(part.name) && IsHtmlElement(element));

  if (part.name_space == SelectorPart::NamespaceMatch::Named) {
    return false;
  }
  if (part.name_space == SelectorPart::NamespaceMatch::Any) {
    // `[*|att]`: the local name in *any* namespace, so the qualified-name
    // lookup every other path uses cannot answer it.
    for (const dom::Attribute& attribute : element.Attributes()) {
      if (attribute.LocalName() == part.name && ValueMatches(part, attribute.value, caseless)) {
        return true;
      }
    }
    return false;
  }
  // `[att]` and `[|att]` are the same selector: an attribute written without a
  // prefix is in no namespace. Matching on the qualified name is what
  // `getAttribute` does and is what an HTML page means.
  const std::string* value = element.GetAttribute(part.name);
  return value != nullptr && ValueMatches(part, *value, caseless);
}

namespace {

// The language of an element: the nearest `lang` (or `xml:lang`) on it or an
// ancestor. Null when nothing declares one, which is not the same as the empty
// string -- an element in no declared language matches no range at all.
const std::string* DeclaredLanguage(const dom::Element& element) {
  for (const dom::Node* at = &element; at != nullptr; at = at->Parent()) {
    if (!at->IsElement()) {
      continue;
    }
    const auto& current = static_cast<const dom::Element&>(*at);
    if (const std::string* lang = current.GetAttribute("lang"); lang != nullptr) {
      return lang;
    }
    if (const std::string* lang = current.GetAttribute("xml:lang"); lang != nullptr) {
      return lang;
    }
  }
  return nullptr;
}

std::vector<std::string_view> Subtags(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t dash = text.find('-', start);
    if (dash == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, dash - start));
    start = dash + 1;
  }
}

// A language *tag* -- what the document declares -- has to be well formed
// before extended filtering can say anything about it: each subtag is one to
// eight alphanumerics. `lang="fr-ninechars"` is not a tag, so `:lang(fr)` does
// not match it, which is the one WPT subtest that catches an implementation
// that skipped this check.
bool WellFormedTag(const std::vector<std::string_view>& subtags) {
  for (const std::string_view subtag : subtags) {
    if (subtag.empty() || subtag.size() > 8) {
      return false;
    }
    for (const char c : subtag) {
      const bool alphanumeric = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
      if (!alphanumeric) {
        return false;
      }
    }
  }
  return true;
}

// RFC 4647 §3.3.2, extended filtering, with the Selectors 4 wildcard.
//
// The subtlety worth naming is step 3D: a subtag of the *tag* may be skipped
// over only when it is longer than one character. A singleton (`x`, `1`) starts
// an extension or a private-use sequence and stops the walk -- so
// `:lang(en-private)` does not match `en-x-private`, while `:lang(en-x)` does,
// because step 3C compares equal before 3D ever asks about skipping.
bool RangeMatchesTag(std::string_view range, const std::vector<std::string_view>& tag) {
  const std::vector<std::string_view> parts = Subtags(range);
  if (parts.empty() || tag.empty()) {
    return false;
  }
  if (parts[0] != "*" && parts[0] != tag[0]) {
    return false;
  }
  std::size_t i = 1;
  std::size_t j = 1;
  while (i < parts.size()) {
    if (parts[i] == "*") {
      ++i;
      continue;
    }
    if (j >= tag.size()) {
      return false;
    }
    if (tag[j] == parts[i]) {
      ++i;
      ++j;
      continue;
    }
    if (tag[j].size() == 1) {
      return false;
    }
    ++j;
  }
  return true;
}

}  // namespace

bool LangSelectorMatches(const dom::Element& element, std::string_view ranges) {
  const std::string* declared = DeclaredLanguage(element);
  if (declared == nullptr) {
    return false;
  }
  const std::string folded = util::AsciiLowerCase(*declared);
  const std::vector<std::string_view> tag = Subtags(folded);
  if (!WellFormedTag(tag)) {
    return false;
  }
  std::size_t start = 0;
  while (start <= ranges.size()) {
    const std::size_t comma = ranges.find(',', start);
    const std::string_view range = comma == std::string_view::npos
                                       ? ranges.substr(start)
                                       : ranges.substr(start, comma - start);
    if (RangeMatchesTag(range, tag)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    start = comma + 1;
  }
  return false;
}

namespace {

// The `<input>` types whose *value* is text the user reads, and therefore the
// text `dir="auto"` looks at. A checkbox has a value too, and nobody reads it.
bool InputValueIsText(const dom::Element& element) {
  const std::string* type = element.GetAttribute("type");
  if (type == nullptr) {
    return true;  // the missing-value default is `text`
  }
  const std::string folded = util::AsciiLowerCase(*type);
  return folded == "text" || folded == "search" || folded == "tel" || folded == "url" ||
         folded == "email" || folded == "password" ||
         // An unknown `type` falls back to the text state, so anything not on
         // the list of non-text states is text.
         !(folded == "hidden" || folded == "checkbox" || folded == "radio" ||
           folded == "submit" || folded == "reset" || folded == "button" ||
           folded == "image" || folded == "color" || folded == "range" ||
           folded == "date" || folded == "month" || folded == "week" ||
           folded == "time" || folded == "datetime-local" || folded == "number" ||
           folded == "file");
}

// UAX #9's rule P2, over the text `dir="auto"` reads.
//
// The walk is the specification's rather than a simplification of it: it skips
// a descendant that has its own `dir` (that subtree's characters are already
// spoken for), skips `<bdi>` (which is its own paragraph by definition), and
// skips `<script>` and `<style>`, whose text is not text. Bounded at 4,096
// bytes, because the answer is decided by the *first* strong character and a
// page that puts none in the first four kilobytes is a page whose direction
// nobody can infer either.
//
// The `<input>` and `<textarea>` cases are the reason this is not a plain tree
// walk: their text is a *value*, not a child, and for `<input>` only some types
// carry text a reader reads -- a checkbox has a value too and nobody reads it.
void CollectAutoText(const dom::Node& node, std::string& out) {
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    if (out.size() >= 4096) {
      return;
    }
    if (child->IsText()) {
      out += static_cast<const dom::Text&>(*child).Data();
      continue;
    }
    if (!child->IsElement()) {
      continue;
    }
    const auto& child_element = static_cast<const dom::Element&>(*child);
    const std::string_view tag = child_element.TagName();
    if (tag == "script" || tag == "style" || tag == "bdi" ||
        child_element.GetAttribute("dir") != nullptr) {
      continue;
    }
    CollectAutoText(child_element, out);
  }
}

}  // namespace

bool AutoDirectionIsRtl(const dom::Element& element) {
  std::string collected;
  const std::string_view tag = element.TagName();
  if (tag == "input") {
    if (!InputValueIsText(element)) {
      return false;
    }
    if (const std::string* value = element.GetAttribute("value")) {
      collected = *value;
    }
  } else if (tag == "textarea") {
    collected = element.TextContent();
  } else {
    CollectAutoText(element, collected);
  }
  std::vector<std::uint32_t> code_points;
  std::size_t at = 0;
  std::uint32_t code = 0;
  while (util::DecodeUtf8(collected, at, code)) {
    code_points.push_back(code);
  }
  return text::ParagraphLevel(code_points) == 1;
}

namespace {

// HTML's directionality algorithm, reduced to the question `:dir()` asks.
// Walks up rather than down: a `dir` attribute an ancestor carries is inherited
// by everything under it, so the answer is the nearest one that says something.
bool ElementIsRtl(const dom::Element& element) {
  for (const dom::Node* at = &element; at != nullptr; at = at->Parent()) {
    if (!at->IsElement()) {
      break;
    }
    const auto& current = static_cast<const dom::Element&>(*at);
    const std::string* dir = current.GetAttribute("dir");
    const std::string folded = dir == nullptr ? std::string() : util::AsciiLowerCase(*dir);
    if (folded == "ltr") {
      return false;
    }
    if (folded == "rtl") {
      return true;
    }
    // `<bdi>` with no `dir`, and `<bdi dir=lol>`, are `auto`. Every other
    // element with an invalid `dir` inherits, which is why the `auto` test is
    // written as "this element is the one that decides" rather than as a value
    // check.
    const bool automatic = folded == "auto" || (current.TagName() == "bdi" && folded != "ltr" &&
                                                folded != "rtl");
    if (automatic) {
      // `ParagraphLevel` already resolves "no strong character" to level zero,
      // which is ltr -- the same default this loop falls out to.
      return AutoDirectionIsRtl(current);
    }
  }
  return false;  // the default direction of a document
}

}  // namespace

bool DirSelectorMatches(const dom::Element& element, std::string_view direction) {
  if (direction != "ltr" && direction != "rtl") {
    // A valid selector that names a direction nothing has. The parser accepted
    // it on purpose -- `:dir(lol)` is a selector and `:dir('ltr')` is not.
    return false;
  }
  return ElementIsRtl(element) == (direction == "rtl");
}

}  // namespace microbrowser::css
