#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "css/StyleSheet.h"
#include "dom/Node.h"

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
    while (start < haystack.size() && (haystack[start] == ' ' || haystack[start] == '\t' ||
                                       haystack[start] == '\n' || haystack[start] == '\f' ||
                                       haystack[start] == '\r')) {
      ++start;
    }
    std::size_t end = start;
    while (end < haystack.size() && haystack[end] != ' ' && haystack[end] != '\t' &&
           haystack[end] != '\n' && haystack[end] != '\f' && haystack[end] != '\r') {
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
          // A functional pseudo-class still counts as a pseudo-class.
          ++result.classes;
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
        case SelectorPart::Kind::Is:
        case SelectorPart::Kind::Not:
          AddSpecificity(result, MostSpecific(part.arguments));
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

bool operator==(const SelectorPart& a, const SelectorPart& b) {
  return a.kind == b.kind && a.name == b.name && a.value == b.value && a.match == b.match &&
         a.arguments == b.arguments && a.nth == b.nth;
}

namespace {

bool AttributeMatches(const SelectorPart& part, const dom::Element& element) {
  const std::string* value = element.GetAttribute(part.name);
  if (value == nullptr) {
    return false;
  }
  switch (part.match) {
    case SelectorPart::AttributeMatch::Exists:
      return true;
    case SelectorPart::AttributeMatch::Equals:
      return *value == part.value;
    case SelectorPart::AttributeMatch::Includes:
      return ContainsWord(*value, part.value);
    case SelectorPart::AttributeMatch::DashMatch:
      return *value == part.value ||
             (value->size() > part.value.size() &&
              value->compare(0, part.value.size(), part.value) == 0 &&
              (*value)[part.value.size()] == '-');
    case SelectorPart::AttributeMatch::Prefix:
      return !part.value.empty() && value->size() >= part.value.size() &&
             value->compare(0, part.value.size(), part.value) == 0;
    case SelectorPart::AttributeMatch::Suffix:
      return !part.value.empty() && value->size() >= part.value.size() &&
             value->compare(value->size() - part.value.size(), part.value.size(), part.value) == 0;
    case SelectorPart::AttributeMatch::Substring:
      return !part.value.empty() && value->find(part.value) != std::string::npos;
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

// The element's 1-based position among its parent's element children, counting
// only same-tag siblings for the of-type family and from the end for the
// `-last-` family. Zero when there is no position to speak of.
std::int64_t SiblingIndex(const dom::Element& element, const NthPattern& nth) {
  const dom::Node* parent = element.Parent();
  if (parent == nullptr) {
    // An element with no parent is its own only sibling, which is the same
    // assumption `:first-child` makes two functions up.
    return 1;
  }
  const std::string_view filter = nth.of_type ? std::string_view(element.TagName())
                                              : std::string_view();
  std::int64_t total = 0;
  std::int64_t position = 0;
  for (const std::unique_ptr<dom::Node>& sibling : parent->Children()) {
    if (ElementMatchesTagFilter(*sibling, filter)) {
      ++total;
      if (sibling.get() == &element) {
        position = total;
      }
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
  const dom::Document* document = element.OwnerDocument();
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

bool MatchesCompound(const CompoundSelector& compound, const dom::Element& element) {
  for (const SelectorPart& part : compound.parts) {
    switch (part.kind) {
      case SelectorPart::Kind::Universal:
        break;
      case SelectorPart::Kind::Type:
        if (element.TagName() != part.name) {
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
      case SelectorPart::Kind::Id: {
        const std::string* id = element.GetAttribute("id");
        if (id == nullptr || *id != part.name) {
          return false;
        }
        break;
      }
      case SelectorPart::Kind::Attribute:
        if (!AttributeMatches(part, element)) {
          return false;
        }
        break;
      case SelectorPart::Kind::Is:
      case SelectorPart::Kind::Where: {
        // Matches if *any* argument matches. The arguments are complex
        // selectors, so each one walks up from this element on its own; that
        // is what makes `:is(.a .b, .c > .d)` mean what it says.
        bool any = false;
        for (const Selector& argument : part.arguments) {
          if (argument.Matches(element)) {
            any = true;
            break;
          }
        }
        if (!any) {
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
        for (const Selector& argument : part.arguments) {
          if (argument.Matches(element)) {
            return false;
          }
        }
        break;
      case SelectorPart::Kind::Nth: {
        const std::int64_t index = SiblingIndex(element, part.nth);
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

// Matches right to left, which is what every engine does: starting from the
// element and walking up is bounded by the tree depth, where starting from the
// leftmost compound would search the whole subtree.
bool MatchesFrom(const std::vector<CompoundSelector>& compounds, std::size_t index,
                 const dom::Element& element) {
  if (!MatchesCompound(compounds[index], element)) {
    return false;
  }
  if (index == 0) {
    return true;
  }

  const CompoundSelector& current = compounds[index];
  const dom::Node* parent = element.Parent();
  switch (current.combinator) {
    case Combinator::None:
    case Combinator::Descendant: {
      for (const dom::Node* at = parent; at != nullptr; at = at->Parent()) {
        if (at->IsElement() &&
            MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*at))) {
          return true;
        }
      }
      return false;
    }
    case Combinator::Child: {
      return parent != nullptr && parent->IsElement() &&
             MatchesFrom(compounds, index - 1, static_cast<const dom::Element&>(*parent));
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
              MatchesFrom(compounds, index - 1, *candidate)) {
            return true;
          }
          previous = candidate;
        }
      }
      if (current.combinator == Combinator::NextSibling) {
        return previous != nullptr && MatchesFrom(compounds, index - 1, *previous);
      }
      return false;
    }
  }
  return false;
}

}  // namespace

bool Selector::Matches(const dom::Element& element) const {
  if (compounds.empty()) {
    return false;
  }
  return MatchesFrom(compounds, compounds.size() - 1, element);
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
