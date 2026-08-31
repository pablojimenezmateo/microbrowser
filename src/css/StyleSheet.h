#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/Animation.h"
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
    Nth,         // :nth-child(An+B [of S]), and its three siblings
    // `:has(rel)`, the one selector whose truth depends on a *subtree* rather
    // than on an ancestor chain. Its arguments are *relative* selectors: the
    // leftmost compound of each carries the combinator that relates it to the
    // element being matched (`Combinator::None` meaning descendant), which is
    // the only place in this model where compound zero's combinator is read.
    // ADR 0016 §1 priced it separately; `bench/CssBenchmarks.cpp` took the
    // measurement and `kMaxHasCandidates` below is what came of it.
    Has,
    // `:scope`. Inside a `:has()` argument it is the element the `:has()` is
    // being asked about; everywhere else it is the document element, which is
    // what a scopeless match is defined to mean.
    Scope,
    // `:lang(en, fr-*)`. The ranges live in `value`, comma-separated, folded to
    // lower case at parse time -- a list rather than a nested selector list, so
    // it costs no member of its own.
    Lang,
    // `:dir(ltr)` / `:dir(rtl)`, with the argument in `value`. Any other ident
    // parses and matches nothing, which is what the grammar says.
    Dir,
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
    // `::before` / `::after` (and the legacy single-colon spellings). They style a
    // box that is not a DOM node -- the originating element still matches the
    // rest of the subject, and layout invents the box when `content` is not
    // `none`/`normal`. youtube's thumbnail aspect hack is empty `::before` with
    // `padding-top` as a percentage of width.
    PseudoElement,
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

  // The `i` and `s` flags of `[att=val i]`. `Default` is neither written, and
  // is *not* a synonym for `Sensitive`: HTML defines a list of attributes whose
  // values are matched ASCII case-insensitively in an HTML document, and the
  // difference between "the author said nothing" and "the author said `s`" is
  // the only thing that can tell those two apart.
  enum class AttributeCase : std::uint8_t { Default, Insensitive, Sensitive };

  // The namespace part written before a `|` on a type or attribute selector.
  // `Default` is no `|` at all -- any namespace for a type (when the sheet has
  // no default `@namespace`) and no namespace for an attribute. `Named` is a
  // prefix the sheet declared (`ns|e`); matching it without a URI would style
  // the wrong elements, so the matcher treats Named as matching nothing and
  // CSSOM still serializes the prefix the author wrote.
  enum class NamespaceMatch : std::uint8_t { Default, Any, None, Named };

  Kind kind = Kind::Universal;
  std::string name;
  std::string value;
  AttributeMatch match = AttributeMatch::Exists;
  AttributeCase attribute_case = AttributeCase::Default;
  NamespaceMatch name_space = NamespaceMatch::Default;
  // Only when `name_space` is `Named`. Empty otherwise.
  std::string ns_prefix;
  // The argument list of `Is`, `Where`, `Not` and `Has`, and the `of S` of
  // `:nth-child(An+B of S)`. Empty for every other kind.
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

// How many elements one `:has()` evaluation may look at before it gives up and
// answers "no".
//
// ADR 0016 §1 said `:has()` "lands last of the five, behind a measurement, and
// if it is expensive it stays behind one". This is that measurement.
// `bench/CssBenchmarks.cpp`, perf build, one selector asked of every element of
// a 4,400-element document, minimum of three runs:
//
//     tree      selector                          ns/element   vs a class
//     wide      `.cell0`                                42.2        1.0x
//     wide      `:has(.cell0)`          (matches)       199.3        4.7x
//     wide      `:has(.absent)`         (no match)      175.8        4.2x
//     deep      `.cell0`                                36.8        1.0x
//     deep      `:has(.cell0)`          (matches)        27.1        0.7x
//     deep      `:has(.absent)`         (no match)     1122.3       30.5x
//
// **Read the ratio column, not the nanoseconds.** This machine is shared, and
// the absolute figures move by 3x between a quiet run and a loaded one; the six
// rows of one run all move together, so the ratio between them is the durable
// half of the measurement. (Take the numbers from one run for the same reason:
// comparing a row measured now against a row measured an hour ago compares two
// machine loads.)
//
// Three things in that table, and none of them was the expected one:
//
//  1. **The shape of the tree decides, not the element count.** Both documents
//     hold 4,400 elements; the wide one is 60x12x6 and the deep one is a spine
//     of 40 wrappers. A descendant search costs the *sum of the subtree sizes*,
//     which is the element count times the average depth -- so the same
//     selector is 30x an ordinary class selector on one document and *cheaper*
//     than one on the other.
//  2. **A failing `:has()` is the expensive one.** A match stops at the first
//     candidate; a miss visits every one. The rule that costs the most is the
//     rule that is doing nothing, which is backwards from the intuition and is
//     why the bound below counts candidates rather than matches.
//  3. **The cascade never sees any of this**, because
//     `StyleResolver::CandidateRules` only offers a rule the elements whose
//     subject compound could match it. A `:has()` written as `.sidebar:has(img)`
//     is asked of the elements with that class and nothing else. Only a subject
//     that states no id, class or tag -- a bare `:has(...)` -- lands in the
//     universal bucket and is asked of the whole document.
//
// So the honest answer is that `:has()` is affordable and needs no performance
// bound: 30x a class selector, on the rule shape a page has to go out of its
// way to write, is not the "too expensive to ship" case the ADR left room for.
// What it does need is a **security** bound, in the sense ADR 0009 uses: a
// stylesheet is attacker-controlled input and a walk with no ceiling on it is a
// hang waiting for a large enough document.
//
// 100,000 is that ceiling and is deliberately far above every real page --
// wikipedia's article DOM is about 19,000 elements and youtube's about 5,000,
// so the largest possible subtree on either is a fifth of it. A bound a real
// page reaches would be a *rendering* difference, and one that changes what a
// page looks like to buy speed is the wrong trade in a project whose priority
// order starts with correctness. Exceeding it answers **no match**, which is
// what an author gets from a selector that finds nothing rather than from one
// that finds everything, and `css.has_bound_hit` counts it -- a silent bound is
// a rendering difference nobody can see from outside.
inline constexpr std::size_t kMaxHasCandidates = 100000;

// Which generated box a selector's subject names, if any. `None` is an ordinary
// element rule; `Before`/`After` are `::before`/`::after`. Layout asks, and the
// cascade filters on it so a `div::before` rule never paints the `div`.
//
// `FirstLetter` is the third and is a different kind of thing from the first
// two: it styles no *new* content, it re-styles the leading characters of text
// that is already there, so layout splits an existing text box rather than
// inventing one. It is here rather than in a second enum because the question
// the cascade asks -- "which of a rule's declarations belong to this element
// and which to something else it originates" -- is the same question for all
// three, and one filter that answers it is one place to get it right.
enum class PseudoElement : std::uint8_t { None, Before, After, FirstLetter };

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

  // `::before` / `::after` on the subject compound, or `None`. Only legal on the
  // subject; a parse that put one elsewhere failed the selector.
  PseudoElement SubjectPseudoElement() const;

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
  // The `@keyframes` blocks, by name and in document order. A later block with the same name replaces
  // an earlier one -- which is the cascade rule for named animations and is not the same as merging
  // them: a page that redefines `spin` means the new one and not both.
  std::vector<KeyframesRule> keyframes;
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

// Prefixes a stylesheet's `@namespace` rules have declared. A selector that
// names a prefix not on this list is invalid. Empty is the cascade default:
// no default namespace, no prefixes -- `ns|e` does not parse.
struct SelectorNamespaces {
  bool has_default = false;
  std::vector<std::string> prefixes;
};

// Parses one selector list ("a, b > c"). Empty when nothing parsed, which is
// how an unsupported selector drops its whole rule rather than matching
// everything. `namespaces` is what CSSOM's `selectorText` needs to keep `*|e`
// when the sheet has a default namespace, and to accept `ns|e` at all.
std::vector<Selector> ParseSelectorList(std::string_view input,
                                        const SelectorNamespaces& namespaces = {});

// `CSS.supports(conditionText)` — the same grammar `@supports` uses, so a page
// and a stylesheet cannot disagree. `SupportsDeclaration` is the two-argument
// form (`CSS.supports(property, value)`).
bool SupportsConditionText(std::string_view condition);

}  // namespace microbrowser::css
