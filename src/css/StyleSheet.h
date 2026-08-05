#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/MediaQuery.h"
#include "css/Token.h"
#include "dom/Node.h"

namespace microbrowser::css {

struct Selector;

// The `An+B` of `:nth-child(An+B)`, and which sequence it counts over.
//
// Kept as two integers rather than as the text, because the match is
// arithmetic: an element at 1-based index `i` matches when `i = An + B` for
// some integer `n >= 0`. `a` and `b` are 32-bit and a stylesheet whose numbers
// do not fit is rejected rather than truncated — a truncated `An+B` is a
// selector that matches a different set of elements than the author wrote.
struct NthPattern {
  std::int32_t a = 0;
  std::int32_t b = 0;
  bool from_end = false;  // :nth-last-child / :nth-last-of-type
  bool of_type = false;   // :nth-of-type / :nth-last-of-type

  bool MatchesIndex(std::int64_t index) const;

  friend bool operator==(const NthPattern&, const NthPattern&) = default;
};

// One simple selector component.
struct SelectorPart {
  enum class Kind : std::uint8_t {
    Type,        // div
    Universal,   // *
    Class,       // .name
    Id,          // #name
    Attribute,   // [name], [name=value], [name~=value], [name|=value]
    PseudoClass, // :hover, :first-child
    // The functional pseudo-classes, which own a nested selector list and make
    // matching recursive. `Is` and `Where` match identically and are separate
    // kinds because their *specificity* differs, and getting that wrong
    // inverts the cascade without anything looking broken: `:is()` and `:not()`
    // take the specificity of their most specific argument, and `:where()`
    // contributes zero — which is the entire reason `:where()` exists.
    Is,          // :is(a, b)
    Where,       // :where(a, b)
    Not,         // :not(a, b)
    Nth,         // :nth-child(An+B), and its three siblings
    // `:host` and `:host(sel)`, which only mean anything in a sheet that belongs
    // to a shadow root: they match the root's *host*, which is an element in a
    // different tree. Matched by the resolver rather than by the matcher, because
    // the matcher is a pure function of (element, selector) and "which root did
    // this rule come from" is neither. ADR 0019 §3.
    Host,
    // `::slotted(sel)`, which matches a node assigned into this scope's slots --
    // again an element from the light DOM, reached from a sheet inside the shadow
    // tree. A pseudo-element in the grammar and a scope question in practice.
    Slotted,
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
  // The argument list of `Is`, `Where` and `Not`. Empty for every other kind.
  std::vector<Selector> arguments;
  NthPattern nth;

  // Not defaulted: `arguments` holds a type that is still incomplete here, so
  // the comparison is written out where `Selector` is whole.
  friend bool operator==(const SelectorPart&, const SelectorPart&);
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

// How deep one selector list may nest inside another before the whole selector
// is rejected. A stylesheet is attacker-controlled input and recursion over it
// is the hazard ADR 0009 bounded for script, so it gets a bound here for the
// same reason.
//
// Measured rather than guessed: across twenty-three stylesheets served by
// reddit, github, wikipedia, MDN and Hacker News — 318KB down to 500 bytes —
// the deepest nesting found was **two**, in
// `:is(:is(.breadcrumbs li) a):not(:hover)`. Eight is four times the observed
// maximum, and far below the stack a recursive descent needs.
inline constexpr int kMaxSelectorNestingDepth = 8;

// A full selector: compounds, leftmost first.
struct Selector {
  std::vector<CompoundSelector> compounds;

  Specificity ComputeSpecificity() const;
  bool Matches(const dom::Element& element) const;

  // The compound that describes the element a rule *styles* -- the last one, since
  // a selector reads left to right and ends at its subject. Null for an empty
  // selector. The scoped cascade asks for it because `:host` and `::slotted()`
  // are only meaningful on the subject: `p :host` selects nothing anywhere.
  const CompoundSelector* Subject() const {
    return compounds.empty() ? nullptr : &compounds.back();
  }

  // Every dynamic state whose value this selector's match depends on, ORed
  // together -- including the ones inside `:is()`, `:where()` and `:not()`, and
  // including the ones in a compound that is not the last, because `li:hover +
  // li` matches on a state that is not on the element it styles.
  //
  // This is the key an invalidation index files the rule under (ADR 0016 §3),
  // and it is a method on the selector rather than a walk in the index so that
  // it cannot disagree with the matcher about which name means which state.
  dom::ElementState DynamicStates() const;

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

// One `@font-face`, as the sheet declares it.
//
// ADR 0024. A descriptor block rather than a rule: it matches nothing and styles
// nothing -- it *adds a face to the font database*, which is why it is a separate
// list on the sheet instead of a StyleRule with an odd selector.
//
// The sources are kept in order and with their declared format, because that
// order is the author's fallback chain: the first one this browser can decode
// wins, and picking any other is downloading a font that will not render.
struct FontFaceSource {
  std::string url;
  // The `format(...)` hint, lowercased, or empty. Advisory -- the bytes decide --
  // but it is what lets a `woff2` entry be skipped without fetching it, which is
  // the whole reason authors write it.
  std::string format;

  friend bool operator==(const FontFaceSource&, const FontFaceSource&) = default;
};

// One `unicode-range` entry, inclusive at both ends.
struct UnicodeRange {
  std::uint32_t first = 0;
  std::uint32_t last = 0;

  friend bool operator==(const UnicodeRange&, const UnicodeRange&) = default;
};

struct FontFace {
  std::string family;
  std::vector<FontFaceSource> sources;
  // 400 and normal unless the descriptors say otherwise. Defaulted rather than
  // optional because a face with no `font-weight` *is* a normal-weight face, and
  // an absent value that meant "any" would make one face answer for all nine.
  int weight = 400;
  bool italic = false;
  // `font-display`, lowercased: "auto", "block", "swap", "fallback", "optional".
  // Kept as text because what it changes is *when the browser paints*, which is
  // the engine's decision rather than the parser's.
  std::string display;
  // The `unicode-range` descriptor, expanded: inclusive code point ranges, with
  // the wildcard form (`U+4??`) already turned into the range it means. Empty
  // means the face claims the whole alphabet, which is what an absent descriptor
  // means.
  //
  // This used to be a bool, and the reason is worth keeping: the descriptor's
  // value cannot be read back out of a reconstructed declaration string --
  // `U+0100-02BA` becomes `U 100 -2BA` once it has been through generic tokens --
  // so the parser recorded only that a range existed. The fix was in the
  // tokenizer, not here: `Token::Kind::UnicodeRange` scans the syntax where the
  // original text still exists.
  std::vector<UnicodeRange> unicode_ranges;

  // Whether this face covers any of the code points a page actually uses. Empty
  // ranges cover everything; an empty *page* is covered by nothing, which is
  // deliberate -- a face is fetched because text needs it.
  bool CoversAnyOf(const std::vector<std::uint32_t>& code_points) const {
    if (unicode_ranges.empty()) {
      return true;
    }
    for (const std::uint32_t code_point : code_points) {
      for (const UnicodeRange& range : unicode_ranges) {
        if (code_point >= range.first && code_point <= range.last) {
          return true;
        }
      }
    }
    return false;
  }

  friend bool operator==(const FontFace&, const FontFace&) = default;
};

struct StyleSheet {
  std::vector<StyleRule> rules;
  // The `@font-face` blocks, in document order. Order matters twice: a later face
  // with the same family and weight replaces an earlier one, and a font stack is
  // resolved against the database as it stands when text is measured.
  std::vector<FontFace> font_faces;
  // Rules and at-rules the parser did not understand. Counted rather than
  // guessed at, for the same reason a filter list's unknown option skips the
  // whole rule: a partially-understood rule applies to requests, or elements,
  // it was never written for.
  std::size_t skipped = 0;
};

// Parses a stylesheet. There is no failure: CSS error recovery is normative,
// and a sheet with a syntax error is one whose bad declarations are dropped and
// whose good ones still apply.
// `context` is what an `@media` prelude is evaluated against, and evaluating it
// here rather than keeping it on the rule is the crude part of this: a sheet
// parsed at one viewport holds the rules that matched then, so a resize has to
// re-parse. `engine::Page` does (see Page::SetViewport). Keeping the condition on
// the rule and asking it during the cascade is the right end state.
//
// A default-constructed context is a zero-sized viewport, which matches
// `max-width` and not `min-width`. That is the answer this function gave for
// *every* parenthesised prelude before the evaluator was wired in, so a caller
// with no viewport -- the user-agent sheet, a test about selectors -- keeps the
// behaviour it had.
StyleSheet ParseStyleSheet(std::string_view input, const MediaContext& context = {});

// Parses the contents of a `style=""` attribute, which is a declaration list
// with no selector.
std::vector<Declaration> ParseDeclarationList(std::string_view input);

// Parses one selector list ("a, b > c"). Empty when nothing parsed, which is
// how an unsupported selector drops its whole rule rather than matching
// everything.
std::vector<Selector> ParseSelectorList(std::string_view input);

}  // namespace microbrowser::css
