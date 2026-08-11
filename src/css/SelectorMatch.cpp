#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "css/SelectorMatchInternal.h"
#include "css/SelectorPredicates.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "util/StringUtil.h"

// Matching a selector against an element, and the specificity that decides
// which of two matching rules wins. Split from the parser because they are
// different jobs over the same shape: this half is a pure function of (element,
// selector) that never sees a token, and it stays that way -- ADR 0016 puts the
// dynamic state on the element rather than in the matcher for the same reason.

namespace microbrowser::css {

namespace {

// Whitespace-separated words of an attribute, for `~=` and for class matching.
bool ContainsWord(std::string_view haystack, std::string_view word) {
  if (word.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start < haystack.size()) {
    while (start < haystack.size() && util::IsHtmlWhitespace(haystack[start])) {
      ++start;
    }
    std::size_t end = start;
    while (end < haystack.size() && !util::IsHtmlWhitespace(haystack[end])) {
      ++end;
    }
    if (end > start && haystack.substr(start, end - start) == word) {
      return true;
    }
    start = end;
  }
  return false;
}

}  // namespace

bool NthPattern::MatchesIndex(std::int64_t index) const {
  // An element at 1-based `index` matches `An+B` when index = a*n + b for some
  // integer n >= 0. Done in 64-bit because `a` and `b` are each a full 32-bit
  // range and `index - b` would otherwise overflow on the difference alone.
  const std::int64_t offset = index - static_cast<std::int64_t>(b);
  if (a == 0) {
    return offset == 0;
  }
  const std::int64_t step = a;
  return offset % step == 0 && offset / step >= 0;
}

namespace {

// The specificity of a selector list is that of its most specific member. Used
// by `:is()` and `:not()`, which take it, and not by `:where()`, which does
// not.
Specificity MostSpecific(const std::vector<Selector>& selectors) {
  Specificity best;
  for (const Selector& selector : selectors) {
    const Specificity candidate = selector.ComputeSpecificity();
    if (best < candidate) {
      best = candidate;
    }
  }
  return best;
}

void AddSpecificity(Specificity& into, const Specificity& from) {
  into.ids += from.ids;
  into.classes += from.classes;
  into.types += from.types;
}

}  // namespace

Specificity Selector::ComputeSpecificity() const {
  Specificity result;
  for (const CompoundSelector& compound : compounds) {
    for (const SelectorPart& part : compound.parts) {
      switch (part.kind) {
        case SelectorPart::Kind::Id:
          ++result.ids;
          break;
        case SelectorPart::Kind::Class:
        case SelectorPart::Kind::Attribute:
        case SelectorPart::Kind::PseudoClass:
          ++result.classes;
          break;
        case SelectorPart::Kind::Nth:
          // A functional pseudo-class still counts as a pseudo-class, plus --
          // for `:nth-child(An+B of S)` -- the specificity of its most specific
          // `of` argument.
          ++result.classes;
          AddSpecificity(result, MostSpecific(part.arguments));
          break;
        case SelectorPart::Kind::Host:
          // `:host` is a pseudo-class, and `:host(sel)` adds its argument's
          // specificity on top -- which is what makes `:host(.wide) p` beat
          // `:host p` inside one shadow sheet.
          ++result.classes;
          AddSpecificity(result, MostSpecific(part.arguments));
          break;
        case SelectorPart::Kind::Slotted:
          // A pseudo-*element*, so it counts as a type rather than as a class.
          ++result.types;
          AddSpecificity(result, MostSpecific(part.arguments));
          break;
        case SelectorPart::Kind::PseudoElement:
          // Same bucket as `::slotted`: a type, not a class.
          ++result.types;
          break;
        case SelectorPart::Kind::Is:
        case SelectorPart::Kind::Not:
        case SelectorPart::Kind::Has:
          // `:has()` takes its argument's specificity and adds nothing of its
          // own, exactly as `:is()` does -- so `.baz` and `:has(.foo)` tie and
          // the later rule wins, which is what `has-specificity.html` asserts.
          AddSpecificity(result, MostSpecific(part.arguments));
          break;
        case SelectorPart::Kind::Scope:
        case SelectorPart::Kind::Lang:
        case SelectorPart::Kind::Dir:
          ++result.classes;  // ordinary pseudo-classes
          break;
        case SelectorPart::Kind::Where:
          break;  // zero, by definition, and the whole point of `:where()`
        case SelectorPart::Kind::Type:
          ++result.types;
          break;
        case SelectorPart::Kind::Universal:
          break;  // the universal selector contributes nothing
      }
    }
  }
  return result;
}

PseudoElement Selector::SubjectPseudoElement() const {
  const CompoundSelector* subject = Subject();
  if (subject == nullptr) {
    return PseudoElement::None;
  }
  for (const SelectorPart& part : subject->parts) {
    if (part.kind == SelectorPart::Kind::PseudoElement) {
      if (part.name == "before") {
        return PseudoElement::Before;
      }
      if (part.name == "after") {
        return PseudoElement::After;
      }
    }
  }
  return PseudoElement::None;
}

bool operator==(const SelectorPart& a, const SelectorPart& b) {
  // Every field, including the two D5 added: `[a=b i]` and `[a=b]` select
  // different elements, so a comparison that called them equal would be a
  // comparison nothing could trust.
  return a.kind == b.kind && a.name == b.name && a.value == b.value && a.match == b.match &&
         a.attribute_case == b.attribute_case && a.name_space == b.name_space &&
         a.arguments == b.arguments && a.nth == b.nth;
}

namespace {

// The context a *nested* list is evaluated in: the same scope, no anchor. An
// argument of `:is()` or `:not()` inside a `:has()` is an ordinary selector
// asked about an ordinary element -- `:has(:is(.a .b))` does not mean the `.a`
// has to be the anchor -- so the anchor is dropped exactly here and nowhere
// else.
MatchContext Unanchored(MatchContext context) {
  context.anchor = nullptr;
  return context;
}

// Whether any selector in a list matches, which is what every nested list
// means. The context is passed on with its anchor cleared by the caller where
// the specification says the argument is not relative.
bool MatchesAnyOf(const std::vector<Selector>& selectors, const dom::Element& element,
                  MatchContext context) {
  if (context.depth > kMaxSelectorNestingDepth) {
    return false;
  }
  ++context.depth;
  for (const Selector& selector : selectors) {
    if (!selector.compounds.empty() &&
        MatchesFrom(selector.compounds, selector.compounds.size() - 1, element, context)) {
      return true;
    }
  }
  return false;
}

bool ElementMatchesTagFilter(const dom::Node& node, std::string_view tag_name) {
  return node.IsElement() &&
         (tag_name.empty() || static_cast<const dom::Element&>(node).TagName() == tag_name);
}

bool HasPreviousElementSibling(const dom::Element& element, std::string_view tag_name) {
  const dom::Node* parent = element.Parent();
  if (parent == nullptr) {
    return false;
  }
  for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
    if (sibling.get() == &element) {
      return false;
    }
    if (ElementMatchesTagFilter(*sibling, tag_name)) {
      return true;
    }
  }
  return false;
}

bool HasFollowingElementSibling(const dom::Element& element, std::string_view tag_name) {
  const dom::Node* parent = element.Parent();
  if (parent == nullptr) {
    return false;
  }
  bool seen = false;
  for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
    if (sibling.get() == &element) {
      seen = true;
      continue;
    }
    if (seen && ElementMatchesTagFilter(*sibling, tag_name)) {
      return true;
    }
  }
  return false;
}

// The element's 1-based position in the sequence `:nth-child()` counts over:
// its parent's element children, narrowed to same-tag siblings for the of-type
// family, or to the ones matching `of S` when the author wrote one, and counted
// from the end for the `-last-` family. Zero when there is no position to speak
// of -- which includes an element that is not itself in the `of S` sequence,
// because `:nth-child(1 of .a)` selects the first `.a` and never a `.b`.
std::int64_t SiblingIndex(const dom::Element& element, const SelectorPart& part,
                          const MatchContext& context) {
  const NthPattern& nth = part.nth;
  const dom::Node* parent = element.Parent();
  if (parent == nullptr) {
    // An element with no parent is its own only sibling, which is the same
    // assumption `:first-child` makes two functions up.
    return part.arguments.empty() || MatchesAnyOf(part.arguments, element, context) ? 1 : 0;
  }
  const std::string_view filter = nth.of_type ? std::string_view(element.TagName())
                                              : std::string_view();
  std::int64_t total = 0;
  std::int64_t position = 0;
  for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
    if (!ElementMatchesTagFilter(*sibling, filter)) {
      continue;
    }
    if (!part.arguments.empty() &&
        !MatchesAnyOf(part.arguments, static_cast<const dom::Element&>(*sibling), context)) {
      continue;
    }
    ++total;
    if (sibling.get() == &element) {
      position = total;
    }
  }
  if (position == 0) {
    return 0;
  }
  return nth.from_end ? total - position + 1 : position;
}

// The three states that are not bits, answered from the one copy of focus that
// lives on the document. ADR 0017 §4 makes focus a document property, and
// session 10 removed the second copy of it; a bit per element would put one
// back, and the failure mode is a `:focus` rule matching an element the next
// keystroke does not go to.
//
// Null for `element`'s document is not an error: a subtree a script has built
// and not inserted has no document, and nothing in it has focus.
bool FocusStateMatches(const dom::Element& element, dom::ElementState state) {
  const dom::Document* document = element.ConnectedDocument();
  if (document == nullptr) {
    return false;
  }
  const dom::Document::FocusState& focus = document->Focus();
  if (focus.element == nullptr) {
    return false;
  }
  if (state == dom::ElementState::FocusWithin) {
    // Walking up from the focused element rather than down over the subtree,
    // which is the same trade Node::ReleaseFocusWithin makes and for the same
    // reason: the depth is bounded and the subtree is not.
    for (const dom::Node* at = focus.element; at != nullptr; at = at->Parent()) {
      if (at == &element) {
        return true;
      }
    }
    return false;
  }
  if (focus.element != &element) {
    return false;
  }
  return state != dom::ElementState::FocusVisible || focus.visible;
}

// Which elements `:disabled`/`:enabled` and `:required`/`:optional` can speak
// about at all. Neither pair is a plain complement: a `<div>` is neither
// enabled nor disabled, and a rule that treated "not disabled" as "enabled"
// would style every element on the page.
bool CanBeDisabled(std::string_view tag) {
  return tag == "button" || tag == "input" || tag == "select" || tag == "textarea" ||
         tag == "optgroup" || tag == "option" || tag == "fieldset";
}

bool CanBeRequired(std::string_view tag) {
  return tag == "input" || tag == "select" || tag == "textarea";
}

// The state a pseudo-class name names, or None when it names none. One table
// rather than a chain of comparisons, because the *invalidation index* has to
// ask the same question of a selector it is filing and a second copy of the
// mapping is how a rule gets filed under a state the matcher answers
// differently.
dom::ElementState StateForPseudoClass(std::string_view name) {
  if (name == "hover") {
    return dom::ElementState::Hover;
  }
  if (name == "active") {
    return dom::ElementState::Active;
  }
  if (name == "target") {
    return dom::ElementState::Target;
  }
  if (name == "checked") {
    return dom::ElementState::Checked;
  }
  if (name == "disabled") {
    return dom::ElementState::Disabled;
  }
  if (name == "required") {
    return dom::ElementState::Required;
  }
  if (name == "placeholder-shown") {
    return dom::ElementState::PlaceholderShown;
  }
  if (name == "focus") {
    return dom::ElementState::Focus;
  }
  if (name == "focus-visible") {
    return dom::ElementState::FocusVisible;
  }
  if (name == "focus-within") {
    return dom::ElementState::FocusWithin;
  }
  return dom::ElementState::None;
}

bool EmptyPseudoClassMatches(const dom::Element& element) {
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsElement() || child->IsText()) {
      return false;
    }
  }
  return true;
}

// The document element of whatever tree `element` sits in, which is what
// `:scope` falls back to when nothing scoped the match.
const dom::Element* RootElementOf(const dom::Element& element) {
  const dom::Node* at = &element;
  while (at->Parent() != nullptr && at->Parent()->IsElement()) {
    at = at->Parent();
  }
  return at->IsElement() ? static_cast<const dom::Element*>(at) : nullptr;
}

bool MatchesCompound(const CompoundSelector& compound, const dom::Element& element,
                     const MatchContext& context) {
  for (const SelectorPart& part : compound.parts) {
    switch (part.kind) {
      case SelectorPart::Kind::Universal:
        if (part.name_space == SelectorPart::NamespaceMatch::None &&
            !element.Namespace().IsNone()) {
          return false;  // `|*`
        }
        break;
      case SelectorPart::Kind::Type:
        if (!TypeSelectorMatches(part, element)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Has:
        if (!HasMatches(part, element, context)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Scope: {
        const dom::Element* scope =
            context.scope != nullptr ? context.scope : RootElementOf(element);
        if (scope != &element) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Lang:
        if (!LangSelectorMatches(element, part.value)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Dir:
        if (!DirSelectorMatches(element, part.value)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Class: {
        const std::string* classes = element.GetAttribute("class");
        if (classes == nullptr || !ContainsWord(*classes, part.name)) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Host:
      case SelectorPart::Kind::Slotted:
        // Never reached in a correct call. `:host` and `::slotted()` are about
        // which *root* a rule came from, and this function is a pure question
        // about (element, selector) -- ADR 0016's rule, and why the two are
        // answered in StyleResolver where the scope is known. Reaching here means
        // the resolver did not strip the part, so it matches nothing rather than
        // everything.
        return false;
      case SelectorPart::Kind::PseudoElement:
        // The cascade filters on SubjectPseudoElement(); matching ignores the
        // part so `div::before` still asks whether `div` matches.
        break;
      case SelectorPart::Kind::Id: {
        const std::string* id = element.GetAttribute("id");
        if (id == nullptr || *id != part.name) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Attribute:
        if (!AttributeSelectorMatches(part, element)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Is:
      case SelectorPart::Kind::Where: {
        // Matches if *any* argument matches. The arguments are complex
        // selectors, so each one walks up from this element on its own; that
        // is what makes `:is(.a .b, .c > .d)` mean what it says. An empty list
        // is a valid `:is()` that matches nothing -- which is what the parser's
        // forgiving mode leaves behind when every argument was unrecognised.
        if (!MatchesAnyOf(part.arguments, element, Unanchored(context))) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Not:
        // Matches if *no* argument matches. Note what this does to a
        // pseudo-class the engine does not implement: it never matches, so
        // `:not()` of it always does. For `:not(:hover)` that is the right
        // answer — nothing is hovered — and for `:not(:checked)` it is wrong
        // until ADR 0016 §2 makes that state a bit on the element.
        if (MatchesAnyOf(part.arguments, element, Unanchored(context))) {
          return false;
        }
        break;
      case SelectorPart::Kind::Nth: {
        const std::int64_t index = SiblingIndex(element, part, Unanchored(context));
        if (index < 1 || !part.nth.MatchesIndex(index)) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::PseudoClass:
        if (part.name == "root") {
          if (element.Parent() == nullptr ||
              element.Parent()->GetKind() != dom::Node::Kind::Document) {
            return false;
          }
        } else if (part.name == "first-child") {
          if (HasPreviousElementSibling(element, "")) {
            return false;
          }
        } else if (part.name == "last-child") {
          if (HasFollowingElementSibling(element, "")) {
            return false;
          }
        } else if (part.name == "only-child") {
          if (HasPreviousElementSibling(element, "") || HasFollowingElementSibling(element, "")) {
            return false;
          }
        } else if (part.name == "first-of-type") {
          if (HasPreviousElementSibling(element, element.TagName())) {
            return false;
          }
        } else if (part.name == "last-of-type") {
          if (HasFollowingElementSibling(element, element.TagName())) {
            return false;
          }
        } else if (part.name == "only-of-type") {
          if (HasPreviousElementSibling(element, element.TagName()) ||
              HasFollowingElementSibling(element, element.TagName())) {
            return false;
          }
        } else if (part.name == "empty") {
          if (!EmptyPseudoClassMatches(element)) {
            return false;
          }
        } else if (part.name == "link") {
          // Every hyperlink, whether or not it has been followed. This browser
          // has no `:visited`, and that is a privacy decision rather than a
          // missing feature: `:visited` lets a page style a link differently
          // depending on the user's history, and every mechanism for reading
          // that difference back -- layout size, painted color, timing -- is a
          // history leak. Treating every link as unvisited is the only answer
          // that leaks nothing, so `:link` matches all of them and `:visited`
          // falls through to the never-match case below.
          if (element.TagName() != "a" || element.GetAttribute("href") == nullptr) {
            return false;
          }
        } else if (part.name == "disabled" || part.name == "enabled") {
          // Neither is the complement of the other: `:enabled` matches a *form
          // control* that is not disabled, so a `<div>` matches neither -- and
          // a `<div disabled>` matches neither either, which is what the tag
          // test is for. The list is the one CSS Selectors 4 names, and it is
          // here for the same reason `:link`'s `a[href]` test is: the
          // selector's definition is written in HTML's vocabulary and the
          // matcher is where it is applied.
          const bool disabled = element.HasState(dom::ElementState::Disabled);
          if (!CanBeDisabled(element.TagName()) || disabled != (part.name == "disabled")) {
            return false;
          }
        } else if (part.name == "required" || part.name == "optional") {
          const bool required = element.HasState(dom::ElementState::Required);
          if (!CanBeRequired(element.TagName()) || required != (part.name == "required")) {
            return false;
          }
        } else if (const dom::ElementState state = StateForPseudoClass(part.name);
                   state != dom::ElementState::None) {
          // The dynamic states of ADR 0016 §2. A bit read, or -- for the three
          // focus states -- one question asked of the document that owns the
          // one copy of focus. Either way the matcher stays a pure function of
          // (element, selector) and does not know that a mouse exists.
          const bool matches = Any(state & dom::kStoredElementStates)
                                   ? element.HasState(state)
                                   : FocusStateMatches(element, state);
          if (!matches) {
            return false;
          }
        } else {
          // A pseudo-class we do not implement must not match. Matching would
          // apply a rule the author scoped to a state we cannot observe.
          return false;
        }
        break;
    }
  }
  return true;
}

}  // namespace

// Matches right to left, which is what every engine does: starting from the
// element and walking up is bounded by the tree depth, where starting from the
// leftmost compound would search the whole subtree.
bool MatchesFrom(const std::vector<CompoundSelector>& compounds, std::size_t index,
                 const dom::Element& element, MatchContext context) {
  if (!MatchesCompound(compounds[index], element, context)) {
    return false;
  }
  if (index == 0) {
    // The leftmost compound. In an ordinary selector there is nothing left to
    // check; in a *relative* one -- a `:has()` argument -- what is left is the
    // relation to the anchor, and it is the only thing that makes
    // `:has(> .b)` different from `:has(.b)`.
    return context.anchor == nullptr ||
           AnchoredBy(element, compounds[0].combinator, *context.anchor);
  }

  const CompoundSelector& current = compounds[index];
  const dom::Node* parent = element.Parent();
  switch (current.combinator) {
    case Combinator::None:
    case Combinator::Descendant: {
      for (const dom::Node* at = parent; at != nullptr; at = at->Parent()) {
        if (at->IsElement() &&
            MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*at), context)) {
          return true;
        }
      }
      return false;
    }
    case Combinator::Child: {
      return parent != nullptr && parent->IsElement() &&
             MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*parent), context);
    }
    case Combinator::NextSibling:
    case Combinator::LaterSibling: {
      if (parent == nullptr) {
        return false;
      }
      const dom::Element* previous = nullptr;
      for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
        if (sibling.get() == &element) {
          break;
        }
        if (sibling->IsElement()) {
          const dom::Element* candidate = static_cast<const dom::Element*>(sibling.get());
          if (current.combinator == Combinator::LaterSibling &&
              MatchesFrom(compounds, index - 1, *candidate, context)) {
            return true;
          }
          previous = candidate;
        }
      }
      if (current.combinator == Combinator::NextSibling) {
        return previous != nullptr && MatchesFrom(compounds, index - 1, *previous, context);
      }
      return false;
    }
  }
  return false;
}

bool Selector::Matches(const dom::Element& element) const {
  if (compounds.empty()) {
    return false;
  }
  return MatchesFrom(compounds, compounds.size() - 1, element, MatchContext{});
}

namespace {

// Bounded by the same constant the parser is: a selector list nested inside a
// selector list is attacker-controlled input, and this walk recurses over the
// shape the parser produced. The parser refuses to build one deeper than
// kMaxSelectorNestingDepth, so this bound can only be reached by a selector
// assembled in code -- and stopping is the right answer there too, because the
// alternative is a stack overflow reachable from a stylesheet.
dom::ElementState StatesIn(const std::vector<CompoundSelector>& compounds, int depth) {
  dom::ElementState states = dom::ElementState::None;
  if (depth > kMaxSelectorNestingDepth) {
    return states;
  }
  for (const CompoundSelector& compound : compounds) {
    for (const SelectorPart& part : compound.parts) {
      if (part.kind == SelectorPart::Kind::PseudoClass) {
        states |= StateForPseudoClass(part.name);
        // `:enabled` and `:optional` are the complements of two of the bits and
        // depend on them, which the name table does not say because the matcher
        // answers them from a tag list as well.
        if (part.name == "enabled") {
          states |= dom::ElementState::Disabled;
        } else if (part.name == "optional") {
          states |= dom::ElementState::Required;
        }
        continue;
      }
      for (const Selector& argument : part.arguments) {
        states |= StatesIn(argument.compounds, depth + 1);
      }
    }
  }
  return states;
}

}  // namespace

dom::ElementState Selector::DynamicStates() const { return StatesIn(compounds, 0); }

}  // namespace microbrowser::css
