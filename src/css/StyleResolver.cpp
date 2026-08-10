#include "css/StyleResolver.h"

#include <algorithm>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssText.h"
#include "text/Bidi.h"
#include "util/StringUtil.h"
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

bool IsIdentContinue(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool SubstituteVarsDepth(std::string_view value, const ComputedStyle& style, int depth,
                         std::string& out) {
  if (depth > kMaxVarDepth) {
    return false;
  }
  for (std::size_t i = 0; i < value.size();) {
    // `var(` only where it starts a function. An ident character before it
    // means this is the tail of a longer name (`xvar(`), not a function. A
    // math operator must *not* block it: youtube sizes the watch player with
    // `calc(var(--h)/var(--w)*100%)` and the `/` is a separator, not part of
    // a name. Restricting the lookbehind to whitespace/`(`/`,` left every
    // `/var(...)` and `*var(...)` unsubstituted, so the calc never applied
    // and `#player-container-inner` stayed height zero.
    if (value.compare(i, 4, "var(") != 0 || (i > 0 && IsIdentContinue(value[i - 1]))) {
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

// P2 over an element's own text: true when the first strong character is right-to-left.
//
// This is `dir="auto"`, and the walk is the specification's rather than a simplification of it: it
// skips a descendant that has its own `dir`, skips `<bdi>` (which is its own paragraph by
// definition), and skips `<script>` and `<style>` (whose text is not text). Bounded at 4,096 bytes,
// because the answer is decided by the *first* strong character and a page that puts none in the
// first four kilobytes of a comment is a page whose direction nobody can infer either.
bool DirectionFromContent(const dom::Element& element) {
  std::string collected;
  const auto walk = [&collected](const dom::Node& node, auto& self) -> void {
    for (const std::unique_ptr<dom::Node>& child : node.Children()) {
      if (collected.size() >= 4096) {
        return;
      }
      if (child->IsText()) {
        collected += static_cast<const dom::Text&>(*child).Data();
        continue;
      }
      if (!child->IsElement()) {
        continue;
      }
      const auto* child_element = static_cast<const dom::Element*>(child.get());
      const std::string_view child_tag = child_element->TagName();
      if (child_tag == "script" || child_tag == "style" || child_tag == "bdi" ||
          child_element->GetAttribute("dir") != nullptr) {
        continue;
      }
      self(*child_element, self);
    }
  };
  walk(element, walk);
  std::vector<std::uint32_t> code_points;
  std::size_t at = 0;
  std::uint32_t code = 0;
  while (util::DecodeUtf8(collected, at, code)) {
    code_points.push_back(code);
  }
  return text::ParagraphLevel(code_points) == 1;
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
  // `dir`, on any element. It is a presentational attribute rather than something the user-agent
  // sheet can express, because its value *is* the property's value -- and it is how nearly every
  // right-to-left document on the web declares itself: `<html dir="rtl">`.
  //
  // `dir="auto"` is UAX #9's P2 applied to this element's own text, which is what makes a comment
  // field holding user-supplied text lay out the way its author wrote it rather than the way the page
  // around it runs. `<bdi>` defaults to it, and that is the element's whole purpose.
  const std::string* dir = element.GetAttribute("dir");
  const std::string lowered = dir == nullptr ? std::string() : Lowered(Trim(*dir));
  if (lowered == "rtl" || lowered == "ltr") {
    add("direction", lowered);
  } else if (lowered == "auto" || (dir == nullptr && tag == "bdi")) {
    add("direction", DirectionFromContent(element) ? "rtl" : "ltr");
  }

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
      "pointer-events",
      // Paint-only: changes alpha of already-laid-out boxes, never their size.
      "opacity",
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

namespace {

// Where a rule is filed, so that an element can find it without every element
// having to look at every rule.
//
// The key is a *necessary* condition for the match, never a sufficient one: a
// rule under `by_class["title"]` still has its whole selector evaluated. So the
// only way to be wrong here is to file a rule somewhere an element that it
// matches will not look -- which is why anything the subject does not state
// outright falls through to the universal list.
//
// Id beats class beats tag on selectivity, which is the order pages are written
// in: `#masthead .item` narrows to one element, `div .item` narrows to a third
// of the document.
enum class BucketKind { Id, Class, Tag, Universal };

struct Bucket {
  BucketKind kind = BucketKind::Universal;
  std::string_view name;
};

Bucket BucketFor(const Selector& selector) {
  const CompoundSelector* subject = selector.Subject();
  if (subject == nullptr) {
    return {};
  }
  Bucket best;
  for (const SelectorPart& part : subject->parts) {
    switch (part.kind) {
      case SelectorPart::Kind::Id:
        // Nothing is more selective, so this one ends the search.
        return {BucketKind::Id, part.name};
      case SelectorPart::Kind::Class:
        if (best.kind != BucketKind::Class) {
          best = {BucketKind::Class, part.name};
        }
        break;
      case SelectorPart::Kind::Type:
        if (best.kind == BucketKind::Universal) {
          best = {BucketKind::Tag, part.name};
        }
        break;
      case SelectorPart::Kind::Host:
      case SelectorPart::Kind::Slotted:
        // Both match an element in a *different* tree from the rule, chosen by
        // the scope rather than by anything the compound says about the element
        // itself. Filing them by the tag beside them would hide a component's
        // `:host` from its own host.
        return {};
      case SelectorPart::Kind::PseudoElement:
        // Does not name the originating element; the other parts of the subject
        // still do, so keep searching.
        break;
      case SelectorPart::Kind::Universal:
      case SelectorPart::Kind::Attribute:
      case SelectorPart::Kind::PseudoClass:
      case SelectorPart::Kind::Is:
      case SelectorPart::Kind::Where:
      case SelectorPart::Kind::Not:
      case SelectorPart::Kind::Nth:
        // None of these narrows to a name the element carries. `:is(.a, .b)`
        // could be split across two buckets and is not: a rule filed in two
        // places is a rule that can be collected twice, and the declaration
        // would then be applied twice at the same specificity.
        break;
    }
  }
  return best;
}

// Whitespace-separated words of a `class` attribute. Kept here rather than
// shared with the matcher's ContainsWord: that one answers a membership
// question, this one enumerates, and folding them together would make the hot
// path allocate.
template <typename Fn>
void ForEachClassWord(std::string_view classes, Fn&& fn) {
  std::size_t at = 0;
  while (at < classes.size()) {
    while (at < classes.size() && util::IsHtmlWhitespace(classes[at])) {
      ++at;
    }
    const std::size_t begin = at;
    while (at < classes.size() && !util::IsHtmlWhitespace(classes[at])) {
      ++at;
    }
    if (at > begin) {
      fn(classes.substr(begin, at - begin));
    }
  }
}

}  // namespace

void StyleResolver::AddStyleSheet(const StyleSheet& sheet, Origin origin,
                                  const dom::Node* scope) {
  ++generation_;
  // Cascade identity changed: every cached answer is about a different rule
  // set. Cleared rather than stamped so memory does not grow with every sheet
  // arrival on a long-lived page.
  style_cache_.clear();
  cache_generation_ = generation_;
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

      const auto position = static_cast<std::uint32_t>(rules_.size());
      const Bucket bucket = BucketFor(selector);
      switch (bucket.kind) {
        case BucketKind::Id:
          index_.by_id[std::string(bucket.name)].push_back(position);
          break;
        case BucketKind::Class:
          index_.by_class[std::string(bucket.name)].push_back(position);
          break;
        case BucketKind::Tag:
          index_.by_tag[std::string(bucket.name)].push_back(position);
          break;
        case BucketKind::Universal:
          index_.universal.push_back(position);
          break;
      }
      rules_.push_back(std::move(entry));
    }
  }
}

void StyleResolver::CandidateRules(const dom::Element& element,
                                   std::vector<std::uint32_t>& out) const {
  out.clear();
  const auto append = [&out](const std::vector<std::uint32_t>& bucket) {
    out.insert(out.end(), bucket.begin(), bucket.end());
  };

  append(index_.universal);
  if (const auto tag = index_.by_tag.find(element.TagName()); tag != index_.by_tag.end()) {
    append(tag->second);
  }
  if (const std::string* id = element.GetAttribute("id")) {
    if (const auto found = index_.by_id.find(*id); found != index_.by_id.end()) {
      append(found->second);
    }
  }
  if (const std::string* classes = element.GetAttribute("class")) {
    ForEachClassWord(*classes, [&](std::string_view word) {
      if (const auto found = index_.by_class.find(word); found != index_.by_class.end()) {
        append(found->second);
      }
    });
  }

  // Document order, which is what the cascade's last tiebreak compares. The
  // sort also collapses the one case that can produce a duplicate: an element
  // whose `class` attribute repeats a word.
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
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
                                      const ComputedStyle& parent,
                                      std::uint64_t parent_style_id,
                                      std::uint64_t* out_style_id) const {
  if (out_style_id != nullptr) {
    *out_style_id = 0;
  }
  if (cache_generation_ != generation_) {
    style_cache_.clear();
    cache_generation_ = generation_;
  }

  const dom::Document* document = element.OwnerDocument();
  const std::uint64_t structure =
      document != nullptr ? document->StructureVersion() : 0;
  const std::uint32_t attr = element.AttrVersion();
  const dom::ElementState state = element.State();

  const auto cached = style_cache_.find(&element);
  if (cached != style_cache_.end() && cached->second.cascade_generation == generation_ &&
      cached->second.structure_version == structure && cached->second.attr_version == attr &&
      cached->second.state == state && cached->second.parent_style_id == parent_style_id) {
    AddPerformanceCounter(PerfCounterId::CssStyleCacheHits);
    ComputedStyle style = cached->second.style;
    if (out_style_id != nullptr) {
      *out_style_id = cached->second.style_id;
    }
    // Adjuster after the hit: transitions observe the cascaded value each time.
    if (adjuster_ != nullptr) {
      adjuster_->AdjustStyle(element, style);
    }
    return style;
  }
  AddPerformanceCounter(PerfCounterId::CssStyleCacheMisses);
  AddPerformanceCounter(PerfCounterId::CssStylesResolved);

  // Inherited properties start from the parent; everything else starts at its
  // initial value. Doing this by construction rather than by a per-property
  // `inherit` check is what makes the resolve one pass.
  ComputedStyle style;
  InheritInto(parent, style);

  // The style attribute participates in the cascade rather than being applied
  // after it. Applied afterwards, it would beat an `!important` author rule,
  // which is backwards: importance is compared *before* specificity, and the
  // style attribute is author-origin with a specificity above any selector.
  std::vector<Declaration> inline_declarations;
  if (const std::string* inline_style = element.GetAttribute("style")) {
    AddPerformanceCounter(PerfCounterId::CssInlineStyleParses);
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

  // Only the rules whose subject could name this element, rather than all of
  // them. See RuleIndex: the list is in document order and every rule on it
  // still gets its whole selector evaluated.
  //
  // The scratch vector is a member of the call rather than of the resolver on
  // purpose -- StyleFor is const and is the function a future parallel cascade
  // would run on several elements at once, and a shared buffer is the one thing
  // that would stop it.
  std::vector<std::uint32_t> candidates;
  CandidateRules(element, candidates);

  std::vector<Candidate> ordered;
  for (const std::uint32_t position : candidates) {
    const Entry& entry = rules_[position];
    AddPerformanceCounter(PerfCounterId::CssCandidatesTested);
    if (entry.selector.SubjectPseudoElement() != PseudoElement::None) {
      // `div::before` styles the generated box, not the `div`.
      continue;
    }
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
  AddPerformanceCounter(PerfCounterId::CssDeclarationsCascaded, ordered.size());
  // A *view* per declaration, not a copy. The overwhelming majority of values
  // contain no `var()` and come out of this loop byte-identical to the rule's
  // own text -- 341,470 of 393,210 on en.wikipedia.org/wiki/CSS -- and copying
  // each one into a fresh string was an allocation, a copy and a free per
  // declaration per element per cascade pass, to hand back what was already
  // there. The rules outlive the call, so the views are safe by construction.
  //
  // The ones that *were* substituted need somewhere to live, and that is
  // `owned`: a deque rather than a vector because `substituted` points into it
  // and a vector would invalidate every one of those on its next growth.
  std::vector<std::string_view> substituted(ordered.size());
  std::deque<std::string> owned;
  std::vector<bool> usable(ordered.size(), true);
  std::vector<std::string_view> unset_properties;
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
      AddPerformanceCounter(PerfCounterId::CssVarSubstitutions);
      substituted[i] = owned.emplace_back(std::move(out));
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
    ApplyDeclaration(declaration.property, substituted[i], parent, style, media_context_);
  }
  // What `rem` is a multiple of, fixed here and inherited from here down. The root element is the
  // one whose own `font-size` decides it, and a `rem` written *on* the root resolves against the
  // initial 16px instead -- which is what the inherited value already was when this element's
  // declarations ran, so the order is the specification's without a special case for it.
  if (element.Parent() == nullptr || !element.Parent()->IsElement()) {
    style.root_font_size = style.font_size;
  }
  // The animation pass, last, because a running transition's value is what everything downstream must
  // see -- layout, paint and `getComputedStyle` alike. Null for a resolver with no engine behind it,
  // which is every test about selectors.
  //
  // Cached *before* the adjuster: the memo is the cascade answer. AdjustStyle
  // still runs on every resolve (hit or miss) so transitions keep observing.
  const std::uint64_t style_id = next_style_id_++;
  style_cache_[&element] = StyleCacheEntry{
      .style = style,
      .cascade_generation = generation_,
      .structure_version = structure,
      .attr_version = attr,
      .state = state,
      .parent_style_id = parent_style_id,
      .style_id = style_id,
  };
  if (out_style_id != nullptr) {
    *out_style_id = style_id;
  }
  if (adjuster_ != nullptr) {
    adjuster_->AdjustStyle(element, style);
  }
  return style;
}

ComputedStyle StyleResolver::StyleForPseudo(const dom::Element& element, PseudoElement which,
                                            const ComputedStyle& originating) const {
  ComputedStyle style;
  InheritInto(originating, style);
  if (which == PseudoElement::None) {
    return style;
  }

  struct Candidate {
    const Declaration* declaration;
    Origin origin;
    Specificity specificity;
    std::size_t order;
  };

  const dom::Node* element_scope = ScopeOf(element);
  std::vector<std::uint32_t> candidates;
  CandidateRules(element, candidates);

  std::vector<Candidate> ordered;
  for (const std::uint32_t position : candidates) {
    const Entry& entry = rules_[position];
    if (entry.selector.SubjectPseudoElement() != which) {
      continue;
    }
    if (!ScopeAdmits(entry, element, element_scope)) {
      continue;
    }
    for (const Declaration& declaration : entry.declarations) {
      ordered.push_back(Candidate{&declaration, entry.origin, entry.specificity, entry.order});
    }
  }

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
    const Declaration& declaration = *candidate.declaration;
    if (declaration.property.rfind("--", 0) == 0) {
      style.SetCustomProperty(declaration.property, Trim(declaration.value).empty()
                                                        ? std::string()
                                                        : declaration.value);
    }
  }
  for (const Candidate& candidate : ordered) {
    const Declaration& declaration = *candidate.declaration;
    if (declaration.property.rfind("--", 0) == 0) {
      continue;
    }
    // Generated content rarely uses `var()`; substitute only when needed so a
    // missing var still unsets rather than applying the literal text.
    if (declaration.value.find("var(") == std::string::npos) {
      ApplyDeclaration(declaration.property, declaration.value, originating, style, media_context_);
      continue;
    }
    std::string out;
    if (SubstituteVarsDepth(declaration.value, style, 0, out)) {
      ApplyDeclaration(declaration.property, out, originating, style, media_context_);
    }
  }
  return style;
}

bool SubstituteVars(std::string_view value, const ComputedStyle& style, std::string& out) {
  return SubstituteVarsDepth(value, style, 0, out);
}

}  // namespace microbrowser::css
