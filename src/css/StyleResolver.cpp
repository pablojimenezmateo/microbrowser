#include "css/StyleResolver.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssText.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::css {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// How many times a substitution may expand a value before it is called a
// cycle. `--a: var(--b)` and `--b: var(--a)` reference each other legally at
// parse time and only fail when one is used, so the guard has to be here
// rather than at the declaration. A depth bound rather than a visited-set
// because the same property may legitimately appear twice in one value --
// `margin: var(--x) var(--x)` is not a cycle.
constexpr int kMaxVarDepth = 32;

// Splits at the top-level comma that separates a `var()`'s name from its
// fallback, so that a fallback which is itself a `var()` with its own comma
// stays in one piece. Npos when there is none.
std::size_t FallbackComma(std::string_view inside) {
  int depth = 0;
  for (std::size_t i = 0; i < inside.size(); ++i) {
    if (inside[i] == '(') {
      ++depth;
    } else if (inside[i] == ')') {
      --depth;
    } else if (inside[i] == ',' && depth == 0) {
      return i;
    }
  }
  return std::string_view::npos;
}

bool SubstituteVarsDepth(std::string_view value, const ComputedStyle& style, int depth,
                         std::string& out);

// The body of one `var(...)`, from just past the opening parenthesis. Appends
// what it resolves to. False when the name is unset and there is no fallback.
bool ResolveOneVar(std::string_view inside, const ComputedStyle& style, int depth,
                   std::string& out) {
  const std::size_t comma = FallbackComma(inside);
  const std::string_view name = Trim(inside.substr(0, comma));
  const bool has_fallback = comma != std::string_view::npos;
  const std::string_view fallback =
      has_fallback ? Trim(inside.substr(comma + 1)) : std::string_view();

  if (const std::string* found = style.CustomProperty(name)) {
    // The referenced value may itself contain references, and it is resolved
    // against *this* element -- which is what makes a custom property set on a
    // child override the one it inherited, even inside a value written on the
    // parent.
    return SubstituteVarsDepth(*found, style, depth + 1, out);
  }
  if (!has_fallback) {
    return false;
  }
  // An empty fallback is a legal fallback: `var(--x,)` is the empty value.
  return SubstituteVarsDepth(fallback, style, depth + 1, out);
}

bool SubstituteVarsDepth(std::string_view value, const ComputedStyle& style, int depth,
                         std::string& out) {
  if (depth > kMaxVarDepth) {
    return false;
  }
  for (std::size_t i = 0; i < value.size();) {
    // `var(` only where it starts a function, so a `--foo` inside a string or
    // an identifier like `sidebar(` is left alone.
    if (value.compare(i, 4, "var(") != 0 ||
        (i > 0 && !IsCssWhitespace(value[i - 1]) && value[i - 1] != '(' &&
                      value[i - 1] != ',')) {
      out.push_back(value[i++]);
      continue;
    }
    std::size_t j = i + 4;
    int depth_parens = 1;
    while (j < value.size() && depth_parens > 0) {
      depth_parens += value[j] == '(' ? 1 : (value[j] == ')' ? -1 : 0);
      ++j;
    }
    if (depth_parens != 0) {
      return false;  // unterminated: not a value anybody wrote on purpose
    }
    if (!ResolveOneVar(value.substr(i + 4, j - i - 5), style, depth, out)) {
      return false;
    }
    i = j;
  }
  return true;
}

bool IsTablePart(std::string_view tag_name) {
  return tag_name == "table" || tag_name == "tr" || tag_name == "td" || tag_name == "th" ||
         tag_name == "thead" || tag_name == "tbody" || tag_name == "tfoot" ||
         tag_name == "col" || tag_name == "colgroup";
}

// An HTML attribute that means a CSS declaration, as the value the attribute
// carries. Empty means the attribute does not apply to this element or does not
// parse, and no declaration is produced.
//
// These are *presented hints*, not author rules: they sit at the very bottom of
// the author origin, below any stylesheet and below the style attribute, so
// `td { text-align: left }` beats `<td align=right>`. The cascade below places
// them there by giving them zero specificity and document order zero.
std::string PresentationalLengthValue(std::string_view attribute_value) {
  const std::string_view trimmed = Trim(attribute_value);
  if (trimmed.empty()) {
    return {};
  }
  // A bare number is pixels and a trailing `%` is a percentage. Anything else --
  // including the legacy `100*` column syntax -- is not a length this browser
  // has, so it produces nothing rather than a guess.
  const bool percent = trimmed.back() == '%';
  const std::string_view digits = percent ? trimmed.substr(0, trimmed.size() - 1) : trimmed;
  if (digits.empty() ||
      digits.find_first_not_of("0123456789") != std::string_view::npos) {
    return {};
  }
  return std::string(digits) + (percent ? "%" : "px");
}

// `align` on a cell or a row is text alignment. On anything else it is a
// float or a legacy centering behaviour, neither of which this maps: producing
// `text-align` for `<img align=left>` would indent the following text instead
// of wrapping it around the image, which is worse than doing nothing.
std::string PresentationalAlignValue(std::string_view tag_name, std::string_view value) {
  if (tag_name != "td" && tag_name != "th" && tag_name != "tr" && tag_name != "thead" &&
      tag_name != "tbody" && tag_name != "tfoot" && tag_name != "col" &&
      tag_name != "colgroup") {
    return {};
  }
  const std::string lowered = Lowered(Trim(value));
  if (lowered == "left" || lowered == "right" || lowered == "center" || lowered == "justify") {
    return lowered;
  }
  return {};
}

// The declarations an element's presentational attributes stand for.
std::vector<Declaration> PresentationalDeclarations(const dom::Element& element) {
  const std::string_view tag = element.TagName();
  std::vector<Declaration> declarations;
  const auto add = [&declarations](const char* property, std::string value) {
    if (!value.empty()) {
      declarations.push_back(Declaration{property, std::move(value), false});
    }
  };
  const auto attribute = [&element](const char* name) -> std::string_view {
    const std::string* value = element.GetAttribute(name);
    return value == nullptr ? std::string_view{} : std::string_view{*value};
  };

  if (tag == "body" || IsTablePart(tag)) {
    if (const std::string* bgcolor = element.GetAttribute("bgcolor")) {
      add("background-color", *bgcolor);
    }
  }
  if (IsTablePart(tag) || tag == "img" || tag == "hr" || tag == "iframe") {
    add("width", PresentationalLengthValue(attribute("width")));
    add("height", PresentationalLengthValue(attribute("height")));
  }
  add("text-align", PresentationalAlignValue(tag, attribute("align")));

  if (tag == "table") {
    if (const std::string* border = element.GetAttribute("border")) {
      const std::string length = PresentationalLengthValue(*border);
      // `border=0` is the overwhelmingly common case and means "no rules",
      // which is already the default -- so it produces nothing rather than an
      // explicit zero that would then beat a stylesheet's border.
      if (!length.empty() && length != "0px") {
        add("border", length + " solid gray");
      }
    }
  }
  // `cellpadding` is written on the table and means padding on every cell it
  // contains, so the cell is where it has to be read. A bounded walk: a table
  // nested past the depth the tree builder itself allows cannot exist, and
  // stopping is the right answer for a document that somehow contains one.
  if (tag == "td" || tag == "th") {
    const dom::Node* ancestor = element.Parent();
    for (int depth = 0; ancestor != nullptr && ancestor->IsElement() && depth < 32; ++depth) {
      const auto* ancestor_element = static_cast<const dom::Element*>(ancestor);
      if (ancestor_element->TagName() == "table") {
        if (const std::string* padding = ancestor_element->GetAttribute("cellpadding")) {
          add("padding", PresentationalLengthValue(*padding));
        }
        break;
      }
      ancestor = ancestor_element->Parent();
    }
  }
  // `cellspacing` is deliberately not mapped. It is the gap *between* cells,
  // which is `border-spacing` -- a property this box model does not have, since
  // rows lay their cells out edge to edge. Mapping it to padding would move the
  // gap inside the cell, where it changes what the text wraps at. The common
  // value on the web by a wide margin is 0, which is what already happens.
  return declarations;
}

}  // namespace

StyleResolver::StyleResolver() {
  AddStyleSheet(ParseStyleSheet(UserAgentStyleSheet()), Origin::UserAgent);
}

bool PropertyAffectsLayout(std::string_view property) {
  // Everything not on this list affects layout, including everything this
  // engine does not implement. See the header: the wrong default here is a box
  // that moved and a screen that did not.
  //
  // `outline` is on it because an outline is drawn outside the border box and
  // takes no space -- which is the whole difference between it and a border,
  // and the reason `:focus { outline: ... }` is the one focus rule that costs
  // nothing to apply.
  static constexpr std::string_view kPaintOnly[] = {
      "background",       "background-attachment", "background-clip",
      "background-color", "background-image",      "background-origin",
      "background-position", "background-repeat",  "background-size",
      "border-bottom-color", "border-color",       "border-left-color",
      "border-right-color",  "border-top-color",   "box-shadow",
      "color",            "cursor",                "outline",
      "outline-color",    "outline-offset",        "outline-style",
      "outline-width",    "text-decoration",       "text-decoration-color",
      "text-decoration-line", "text-decoration-style", "text-shadow",
      // `transform` is paint-only, and this is the line that makes a hover worth
      // having: a transformed box occupies exactly the space it would have occupied
      // untransformed, so `a:hover { transform: scale(1.05) }` costs a repaint of a
      // rect rather than a relayout of the page. It is also why the display list
      // carries the matrix instead of layout carrying the geometry.
      "transform",        "transform-origin",
      // `z-index` moves a box between layers and never between positions. It is on
      // this list for the same reason `transform` is: the box occupies the space it
      // already occupied.
      "z-index",
      "visibility",
  };
  return std::find(std::begin(kPaintOnly), std::end(kPaintOnly), property) ==
         std::end(kPaintOnly);
}

void StyleInvalidation::AddRule(const Selector& selector,
                                const std::vector<Declaration>& declarations) {
  const dom::ElementState states = selector.DynamicStates();
  if (!Any(states)) {
    return;
  }
  AddPerformanceCounter(PerfCounterId::CssDynamicRulesIndexed);
  depends_ |= states;
  for (const Declaration& declaration : declarations) {
    if (PropertyAffectsLayout(declaration.property)) {
      layout_ |= states;
      return;
    }
  }
}

StyleChangeEffect StyleInvalidation::EffectOf(dom::ElementState changed) const {
  if (Any(changed & layout_)) {
    return StyleChangeEffect::Layout;
  }
  if (Any(changed & depends_)) {
    return StyleChangeEffect::Paint;
  }
  return StyleChangeEffect::None;
}

void StyleResolver::AddStyleSheet(const StyleSheet& sheet, Origin origin,
                                  const dom::Node* scope) {
  for (const StyleRule& rule : sheet.rules) {
    for (const Selector& selector : rule.selectors) {
      invalidation_.AddRule(selector, rule.declarations);
      Entry entry;
      entry.selector = selector;
      entry.declarations = rule.declarations;
      entry.origin = origin;
      entry.scope = scope;
      entry.specificity = selector.ComputeSpecificity();
      entry.order = next_order_++;
      rules_.push_back(std::move(entry));
    }
  }
}

const dom::Node* StyleResolver::ScopeOf(const dom::Node& node) {
  // The root of the tree `node` is in, when that root is a shadow root. A
  // document-tree element answers null, which is the scope a document sheet
  // carries -- so the common comparison is `nullptr == nullptr` and costs
  // nothing.
  const dom::Element* host = dom::ShadowHostOf(node);
  return host == nullptr ? nullptr : host->ShadowRoot();
}

bool StyleResolver::ScopeAdmits(const Entry& entry, const dom::Element& element,
                                const dom::Node* element_scope) {
  // `:host` and `::slotted()` reach *out* of the scope they were written in, and
  // they are the only two things that do. Handled before the ordinary scope
  // comparison, because for both of them the element is deliberately in a
  // different tree from the rule.
  const CompoundSelector* subject = entry.selector.Subject();
  if (subject != nullptr) {
    for (const SelectorPart& part : subject->parts) {
      if (part.kind == SelectorPart::Kind::Host) {
        // A `:host` rule in a document sheet matches nothing: there is no scope
        // for it to be the host of.
        if (entry.scope == nullptr || entry.scope->GetKind() != dom::Node::Kind::DocumentFragment) {
          return false;
        }
        const dom::Element* host =
            static_cast<const dom::DocumentFragment*>(entry.scope)->Host();
        if (host != &element) {
          return false;
        }
        // `:host(sel)` asks whether `sel` matches the host itself, which is the
        // one case where the argument is evaluated against the subject rather
        // than against something inside it.
        for (const Selector& argument : part.arguments) {
          if (argument.Matches(element)) {
            return true;
          }
        }
        return part.arguments.empty();
      }
      if (part.kind == SelectorPart::Kind::Slotted) {
        // A node assigned into this scope: a child of the host, matched by the
        // argument. Deliberately *not* any descendant -- assignment is one level,
        // which is what makes it answerable without a walk.
        if (entry.scope == nullptr || element.Parent() == nullptr) {
          return false;
        }
        if (ScopeOf(element) != nullptr) {
          return false;  // already inside a shadow tree, so nothing slotted it
        }
        const dom::Element* host =
            entry.scope->GetKind() == dom::Node::Kind::DocumentFragment
                ? static_cast<const dom::DocumentFragment*>(entry.scope)->Host()
                : nullptr;
        if (host == nullptr || element.Parent() != host) {
          return false;
        }
        for (const Selector& argument : part.arguments) {
          if (argument.Matches(element)) {
            return true;
          }
        }
        return false;
      }
    }
  }
  // The ordinary case: a rule applies inside the tree it was written in and
  // nowhere else. A document rule does not reach into a shadow tree, and a
  // component's rule does not leak out of one -- which is the whole of what
  // "scoped" means and the reason a component can use `.title` without asking
  // what else on the page does.
  if (entry.scope != element_scope) {
    return false;
  }
  return entry.selector.Matches(element);
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
  // Custom properties inherit, which is the entire basis of how a modern
  // stylesheet is written: set on `:root` once, referenced everywhere below.
  style.custom_properties = parent.custom_properties;

  // The style attribute participates in the cascade rather than being applied
  // after it. Applied afterwards, it would beat an `!important` author rule,
  // which is backwards: importance is compared *before* specificity, and the
  // style attribute is author-origin with a specificity above any selector.
  std::vector<Declaration> inline_declarations;
  if (const std::string* inline_style = element.GetAttribute("style")) {
    inline_declarations = ParseDeclarationList(*inline_style);
  }
  const std::vector<Declaration> presentational_declarations =
      PresentationalDeclarations(element);

  struct Candidate {
    const Declaration* declaration;
    Origin origin;
    Specificity specificity;
    std::size_t order;
  };

  // Which tree this element is in: the shadow root that contains it, or null for
  // the document. Computed once rather than per rule, because it is the same
  // answer for every entry and a walk to the root per rule is a walk per rule.
  const dom::Node* element_scope = ScopeOf(element);

  std::vector<Candidate> ordered;
  for (const Entry& entry : rules_) {
    if (!ScopeAdmits(entry, element, element_scope)) {
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

  // Two passes, and the split is the specification's own. Custom properties
  // are resolved first, in cascade order, because every other declaration may
  // reference one -- and a reference has to see the winner rather than
  // whichever declaration happens to come before it in this list.
  //
  // They are inherited already: `style` was seeded from the parent, so an
  // element that sets none of its own has its parent's, which is what makes a
  // `--fg` on `:root` reach everything.
  for (const Candidate& candidate : ordered) {
    const Declaration& declaration = *candidate.declaration;
    if (declaration.property.rfind("--", 0) == 0) {
      style.SetCustomProperty(declaration.property, Trim(declaration.value).empty()
                                                        ? std::string()
                                                        : declaration.value);
    }
  }

  // Now substitute. A reference that resolves to nothing and has no fallback
  // makes its declaration **invalid at computed-value time**, which is not the
  // same as unrecognized: the property takes its inherited or initial value,
  // and a lower-priority declaration for it does *not* get to win instead.
  //
  // Since this list is in ascending priority and the last one wins, that rule
  // comes out as: if the winning declaration for a property is invalid, drop
  // every declaration for that property. An invalid one that was going to lose
  // anyway is simply skipped.
  std::vector<std::string> substituted(ordered.size());
  std::vector<bool> usable(ordered.size(), true);
  std::vector<std::string> unset_properties;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    const Declaration& declaration = *ordered[i].declaration;
    if (declaration.property.rfind("--", 0) == 0) {
      usable[i] = false;  // already applied above
      continue;
    }
    if (declaration.value.find("var(") == std::string::npos) {
      substituted[i] = declaration.value;
      continue;
    }
    std::string out;
    if (SubstituteVarsDepth(declaration.value, style, 0, out)) {
      substituted[i] = std::move(out);
    } else {
      usable[i] = false;
      // The winner for this property, so far. Recorded rather than acted on
      // immediately, because a later declaration may still supersede it.
      unset_properties.push_back(declaration.property);
    }
  }
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (!usable[i]) {
      continue;
    }
    const Declaration& declaration = *ordered[i].declaration;
    // Dropped because the declaration that *won* this property was invalid at
    // computed-value time, which unsets the property outright.
    if (std::find(unset_properties.begin(), unset_properties.end(), declaration.property) !=
        unset_properties.end()) {
      bool superseded = false;
      for (std::size_t j = i + 1; j < ordered.size(); ++j) {
        superseded = superseded || (usable[j] && ordered[j].declaration->property ==
                                                     declaration.property);
      }
      if (!superseded) {
        continue;
      }
    }
    const Declaration resolved{declaration.property, substituted[i], declaration.important};
    ApplyDeclaration(resolved, parent, style);
  }
  return style;
}

bool SubstituteVars(std::string_view value, const ComputedStyle& style, std::string& out) {
  return SubstituteVarsDepth(value, style, 0, out);
}

std::string_view UserAgentStyleSheet() {
  // Without this, `<div>` is inline and every document is one long line. The
  // values are the ones the HTML specification's rendering section gives.
  return R"CSS(
html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, li, section, article,
header, footer, nav, aside, main, blockquote, pre, form, figure, figcaption,
hr, dl, dt, dd, fieldset, legend, details, summary, dialog, address, center,
menu, dir, optgroup, hgroup, search, noscript {
  display: block;
}
/* A block, not an inline: a <center> holding a table is the classic 1990s page
   layout, and treating it as inline puts the table's rows on one line. The
   alignment value is the one that also centres block children, which is the
   whole point of the element and which `text-align: center` cannot do. */
center { text-align: -microbrowser-center }
/* The HTML rendering spec resets alignment at a table for exactly this reason:
   `text-align` inherits, so without it a <center> or a centred div would centre
   the text of every cell in every table inside it -- which is not what any page
   wrapping a layout table in <center> is asking for. */
table { text-align: left }
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
head, style, script, title, meta, link, source { display: none }
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
small { font-size: 0.83em }
big { font-size: 1.17em }
code, kbd, samp, tt, pre { font-family: monospace }
a:link { color: #0000EE }
ul, ol { margin: 1em 0; padding-left: 40px }
blockquote { margin: 1em 40px }
pre { white-space: pre; margin: 1em 0 }
hr { margin: 0.5em 0; border-width: 1px; border-color: gray }
)CSS";
}

}  // namespace microbrowser::css
