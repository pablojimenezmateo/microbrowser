#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/Token.h"
#include "dom/Node.h"

namespace microbrowser::css {

// One simple selector component.
struct SelectorPart {
  enum class Kind : std::uint8_t {
    Type,        // div
    Universal,   // *
    Class,       // .name
    Id,          // #name
    Attribute,   // [name], [name=value], [name~=value], [name|=value]
    PseudoClass, // :hover, :first-child
  };

  enum class AttributeMatch : std::uint8_t {
    Exists,
    Equals,
    Includes,     // ~= a whitespace-separated word
    DashMatch,    // |= exactly, or followed by a hyphen
    Prefix,       // ^=
    Suffix,       // $=
    Substring,    // *=
  };

  Kind kind = Kind::Universal;
  std::string name;
  std::string value;
  AttributeMatch match = AttributeMatch::Exists;

  friend bool operator==(const SelectorPart&, const SelectorPart&) = default;
};

enum class Combinator : std::uint8_t {
  None,        // the rightmost compound
  Descendant,  // a b
  Child,       // a > b
  NextSibling, // a + b
  LaterSibling,// a ~ b
};

// A compound selector plus how it joins the one to its left.
struct CompoundSelector {
  Combinator combinator = Combinator::None;
  std::vector<SelectorPart> parts;

  friend bool operator==(const CompoundSelector&, const CompoundSelector&) = default;
};

// Specificity, as three counts. Compared lexicographically, never summed —
// summing is the classic bug that makes eleven classes beat an id.
struct Specificity {
  std::uint32_t ids = 0;
  std::uint32_t classes = 0;
  std::uint32_t types = 0;

  bool operator<(const Specificity& other) const {
    if (ids != other.ids) {
      return ids < other.ids;
    }
    if (classes != other.classes) {
      return classes < other.classes;
    }
    return types < other.types;
  }
  friend bool operator==(const Specificity&, const Specificity&) = default;
};

// A full selector: compounds, leftmost first.
struct Selector {
  std::vector<CompoundSelector> compounds;

  Specificity ComputeSpecificity() const;
  bool Matches(const dom::Element& element) const;

  friend bool operator==(const Selector&, const Selector&) = default;
};

struct Declaration {
  std::string property;
  std::string value;
  bool important = false;

  friend bool operator==(const Declaration&, const Declaration&) = default;
};

struct StyleRule {
  std::vector<Selector> selectors;
  std::vector<Declaration> declarations;

  friend bool operator==(const StyleRule&, const StyleRule&) = default;
};

struct StyleSheet {
  std::vector<StyleRule> rules;
  // Rules and at-rules the parser did not understand. Counted rather than
  // guessed at, for the same reason a filter list's unknown option skips the
  // whole rule: a partially-understood rule applies to requests, or elements,
  // it was never written for.
  std::size_t skipped = 0;
};

// Parses a stylesheet. There is no failure: CSS error recovery is normative,
// and a sheet with a syntax error is one whose bad declarations are dropped and
// whose good ones still apply.
StyleSheet ParseStyleSheet(std::string_view input);

// Parses the contents of a `style=""` attribute, which is a declaration list
// with no selector.
std::vector<Declaration> ParseDeclarationList(std::string_view input);

// Parses one selector list ("a, b > c"). Empty when nothing parsed, which is
// how an unsupported selector drops its whole rule rather than matching
// everything.
std::vector<Selector> ParseSelectorList(std::string_view input);

}  // namespace microbrowser::css
