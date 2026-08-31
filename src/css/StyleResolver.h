#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "css/ComputedStyle.h"
#include "css/StyleSheet.h"
#include "dom/FlatTree.h"
#include "dom/Node.h"
#include "util/TransparentStringHash.h"

namespace microbrowser::css {

// Where a declaration came from. The cascade compares origin *before*
// specificity, which is why this is an ordered enum rather than a label: a
// user-agent rule with a thousand ids still loses to an author rule.
enum class Origin : std::uint8_t {
  UserAgent = 0,
  Author = 1,
  // A `style=""` attribute, which outranks every selector-based author rule
  // regardless of specificity.
  Inline = 2,
};

// What re-resolving the cascade after a change would cost.
//
// Three answers rather than a bool, because the two ADR 0016 §3 protects are
// different properties: `None` is "a hover no rule mentions costs nothing", and
// `Paint` is "a hover that changes only colour does not run layout". Collapsing
// them loses the first, which is the one that decays silently.
enum class StyleChangeEffect : std::uint8_t {
  None,
  Paint,
  Layout,
};

// Which dynamic states the rules in a cascade actually depend on, and which of
// those can move a box.
//
// The index of ADR 0016 §3, in the form the state row of its table needs: a
// rule is filed under every state its selector mentions, and under `layout_`
// as well when any of its declarations can change geometry. A state no rule
// mentions is then a change nothing has to be recomputed for -- which is what
// makes a mouse crossing a page with no `:hover` rules free rather than a full
// cascade.
//
// Deliberately a *union* over every sheet in the cascade rather than one index
// per sheet. The question a change asks is "could anything have changed", and
// answering it per sheet would mean asking it once per sheet.
//
// The other three rows of that table -- a class, an attribute, a DOM edit --
// are not here yet. They need a change signal finer than dom::Document's
// mutation version, which today says only that *something* moved; adding keys
// nothing can query would be an index that is always right and never consulted.
class StyleInvalidation {
 public:
  // Files one rule. Called per rule as a sheet is added, so the cost is
  // proportional to the rule count and paid once.
  void AddRule(const Selector& selector, const std::vector<Declaration>& declarations);

  // What a change to any of `changed` costs. `changed` is a set, because a
  // pointer moving off one element and onto another can flip several states at
  // once and the answer is the most expensive of them.
  StyleChangeEffect EffectOf(dom::ElementState changed) const;

  bool DependsOn(dom::ElementState state) const { return Any(depends_ & state); }

 private:
  dom::ElementState depends_ = dom::ElementState::None;
  dom::ElementState layout_ = dom::ElementState::None;
};

// Whether changing this property can move a box, as opposed to only changing
// what is drawn in one.
//
// **Unknown answers layout**, which is the whole discipline of the table: a
// property nobody classified makes a change slower than it needed to be, where
// the other default would make it *wrong* -- a box that moved and a screen that
// did not. The paint-only list is therefore short and every entry on it is a
// property this engine implements and has checked.
//
// The same table `transition` will need (ADR 0014 step 5), which is why it is
// built once here rather than inside the invalidation index.
bool PropertyAffectsLayout(std::string_view property);

// Resolves computed styles for a document.
//
// The cascade order, in full, because getting it partly right is the usual
// outcome: origin, then `!important` (which *reverses* the origin order), then
// specificity, then document order. Every one of those is tested.
// A last pass over a resolved style, before anything reads it.
//
// Declared here and implemented by `src/engine` -- the same inversion `bindings::GeometrySource` uses
// and for the same reason. What needs it is animation (ADR 0014 §5): a running transition overwrites
// the properties it controls, and the value depends on *what time it is*, which a pure cascade cannot
// know. Putting the hook here rather than making every caller of `StyleFor` remember to call the
// engine is what stops layout and `getComputedStyle` from disagreeing about an element's width mid
// transition.
class StyleAdjuster {
 public:
  virtual ~StyleAdjuster() = default;
  virtual void AdjustStyle(const dom::Element& element, ComputedStyle& style) const = 0;
};

class StyleResolver {
 public:
  StyleResolver();

  // `scope` is the shadow root the sheet belongs to, or null for the document's
  // own sheets. ADR 0019 §3: a tree has an owner, and a rule from a shadow root
  // matches only inside that root -- which is what makes a component's styles
  // *its own* rather than a global change.
  //
  // A `const dom::Node*` rather than a richer type on purpose: what the cascade
  // needs from a scope is identity, and identity is a pointer.
  void AddStyleSheet(const StyleSheet& sheet, Origin origin,
                     const dom::Node* scope = nullptr);

  // What the rules in this cascade depend on. ADR 0016 §3 -- the caller asks
  // this *before* deciding whether a state change is worth recomputing
  // anything, which is the only order in which "costs nothing" can be true.
  const StyleInvalidation& Invalidation() const { return invalidation_; }

  // The animation pass, or null for a resolver with no engine behind it -- which is every test about
  // selectors and the user-agent sheet. Null means the cascade's answer is final, which is what it was
  // before animations existed.
  void SetAdjuster(const StyleAdjuster* adjuster) { adjuster_ = adjuster; }

  // The viewport lengths in declarations resolve against. Set by the engine on
  // every resize; a zero viewport leaves `vw`/`vh` unparsed rather than guessed.
  void SetMediaContext(const MediaContext& context) { media_context_ = context; }

  // The style of one element, given its parent's already-computed style.
  // Passing the parent style rather than looking it up is what makes
  // inheritance a single pass down the tree instead of a walk up per property.
  //
  // `parent_style_id` / `out_style_id` are the style-cache provenance tokens
  // (TD-0021): 0 means "initial / unknown parent". Callers that walk the tree
  // top-down thread the ids so an attribute write on one element does not
  // flush cascade answers for its siblings.
  ComputedStyle StyleFor(const dom::Element& element, const ComputedStyle& parent,
                         std::uint64_t parent_style_id = 0,
                         std::uint64_t* out_style_id = nullptr) const;

  // The style of `element::before`, `element::after` or `element::first-letter`.
  // Inheritance is from the originating element's computed style; there is no
  // style attribute and no presentational hint on a generated box. Returns a
  // style with `content: normal` when nothing matched -- layout treats that as
  // "no box".
  //
  // `out_matched`, when given, says whether any rule actually named this pseudo
  // on this element. `::before` does not need it (`content` is the signal, and
  // a `::before` with no `content` generates nothing), but `::first-letter` has
  // no such property: every declaration on it is optional, so "nothing matched"
  // and "matched and changed nothing" are indistinguishable from the style
  // alone -- and the difference decides whether layout splits a text box.
  ComputedStyle StyleForPseudo(const dom::Element& element, PseudoElement which,
                               const ComputedStyle& originating,
                               bool* out_matched = nullptr) const;

  // Whether any rule in this cascade has `which` as its subject pseudo-element.
  // Answered from a flag set as sheets are added, so a page whose sheets never
  // say `::first-letter` -- which is nearly all of them -- costs one bool test
  // per block box rather than a cascade walk per block box.
  bool AnyRuleTargets(PseudoElement which) const;

  // Computes styles for the whole document, in tree order, and hands each one
  // to `visit(element, style)`.
  template <typename Visitor>
  void ForEachStyledElement(const dom::Document& document, Visitor&& visit) const {
    const ComputedStyle root = InitialStyle();
    for (dom::Node* child : dom::FlatChildren(document)) {
      Walk(*child, root, 0, visit);
    }
  }

  // The style an element inherits when it has no parent element: the initial
  // values, which are what the root inherits from.
  static ComputedStyle InitialStyle() { return ComputedStyle{}; }

  std::size_t RuleCount() const { return rules_.size(); }

  // Bumped on every `AddStyleSheet` -- lets the engine skip a stale box tree (TD-0005).
  std::uint64_t Generation() const { return generation_; }

  // The shadow root `node` is in, or null for the document tree.
  static const dom::Node* ScopeOf(const dom::Node& node);

 private:
  struct Entry {
    Selector selector;
    std::vector<Declaration> declarations;
    Origin origin = Origin::Author;
    // Null for a document sheet. See AddStyleSheet.
    const dom::Node* scope = nullptr;
    Specificity specificity;
    // Position in the sheet, which is the last tiebreak. Two rules that are
    // equal in every other respect are decided by which came later.
    std::size_t order = 0;
  };

  template <typename Visitor>
  void Walk(const dom::Node& node, const ComputedStyle& parent, std::uint64_t parent_style_id,
            Visitor& visit) const {
    ComputedStyle style = parent;
    std::uint64_t style_id = parent_style_id;
    if (node.IsElement()) {
      const auto& element = static_cast<const dom::Element&>(node);
      style = StyleFor(element, parent, parent_style_id, &style_id);
      visit(element, style);
    }
    // The *flattened* tree, so that inheritance crosses a shadow boundary the way
    // the specification says it does -- a slotted node inherits from where it
    // renders, not from where it is written. ADR 0019 §2-3, and the reason the
    // traversal is shared with layout rather than reimplemented: two answers to
    // "what are this node's children for rendering" is the disagreement the ADR
    // refuses to allow.
    for (dom::Node* child : dom::FlatChildren(node)) {
      Walk(*child, style, style_id, visit);
    }
  }

  // Whether `entry` may apply to `element`, scope included. Static because it is
  // a question about the pair and nothing else -- and separate from
  // `Selector::Matches` because *that* is a pure function of (element, selector)
  // and a scope is neither. ADR 0016's rule, kept.
  static bool ScopeAdmits(const Entry& entry, const dom::Element& element,
                          const dom::Node* element_scope);

  // Which rules could conceivably match an element, keyed by the one part of the
  // selector that is cheap to look up.
  //
  // Without this the cascade tested every element against every rule, which is
  // quadratic in the size of the page and was 99% of the time to load
  // youtube.com: 18,360 rules against 686 elements is 12.6 million full selector
  // matches, per layout, seventeen times. A rule can only match an element whose
  // subject compound it describes, so it is filed under the most selective thing
  // that compound states -- its id, else one of its classes, else its tag -- and
  // an element asks only the three or four lists that could name it.
  //
  // `universal` is the escape hatch and it has to exist: a subject that states
  // none of those three (`*`, `[hidden]`, `:hover`, and the two selectors that
  // reach out of their own tree, `:host` and `::slotted()`) is checked against
  // everything. A rule filed wrongly here does not render wrongly *sometimes* --
  // it stops applying entirely -- so the fallback is deliberately the default
  // rather than the exception, and only the three kinds that are certain to be
  // stated on the element get a narrower home.
  template <typename V>
  using ByName =
      std::unordered_map<std::string, V, util::TransparentStringHash, std::equal_to<>>;

  struct RuleIndex {
    ByName<std::vector<std::uint32_t>> by_id;
    ByName<std::vector<std::uint32_t>> by_class;
    ByName<std::vector<std::uint32_t>> by_tag;
    std::vector<std::uint32_t> universal;
  };

  // Appends the indices of every rule that could match `element` to `out`, in
  // ascending document order. Not deduplicated across buckets, because a rule
  // is filed in exactly one of them.
  void CandidateRules(const dom::Element& element, std::vector<std::uint32_t>& out) const;

  std::vector<Entry> rules_;
  // One bit per PseudoElement: has any rule ever named it as its subject. See
  // AnyRuleTargets -- this is what keeps `::first-letter` off the cost of every
  // block on every page that does not use it.
  std::array<bool, 4> targets_pseudo_{};
  RuleIndex index_;
  StyleInvalidation invalidation_;
  const StyleAdjuster* adjuster_ = nullptr;
  MediaContext media_context_;
  std::size_t next_order_ = 0;
  std::uint64_t generation_ = 0;

  // Computed-style cache across BuildBoxTree passes that share a cascade
  // generation and document structure (TD-0021). Mutable because StyleFor is
  // const for callers; the cache is an answer memo, not observable state.
  // Pre-adjuster styles only — AdjustStyle still runs after every hit so
  // transitions keep observing.
  struct StyleCacheEntry {
    ComputedStyle style;
    std::uint64_t cascade_generation = 0;
    std::uint64_t structure_version = 0;
    std::uint32_t attr_version = 0;
    dom::ElementState state = dom::ElementState::None;
    std::uint64_t parent_style_id = 0;
    std::uint64_t style_id = 0;
  };
  mutable std::unordered_map<const dom::Element*, StyleCacheEntry> style_cache_;
  mutable std::uint64_t next_style_id_ = 1;
  mutable std::uint64_t cache_generation_ = 0;
};

// The built-in stylesheet. Every browser has one, and without it `<div>` is
// inline and every document is one long line of text.
//
// Compiled in rather than loaded from disk: it is not user content, and a
// stylesheet the browser reads at startup is a file somebody can replace.
std::string_view UserAgentStyleSheet();

// Applies one declaration to a style. Exposed because it is the single place a
// property name becomes a value, and a property that parses but is not applied
// is invisible without a direct test.
//
// True when the declaration was applied: the property is one this engine
// implements *and* the value is one it understands. False is every other case,
// and the two are deliberately one answer, because CSS makes no distinction --
// an unknown property and a bad value are both dropped, and `@supports` reports
// no for both. This return value is what makes SupportsDeclaration honest: it
// cannot drift from the property table because it *is* the property table.
bool ApplyDeclaration(const Declaration& declaration, const ComputedStyle& parent,
                      ComputedStyle& style, const MediaContext& context = {});

// The same, without a `Declaration` to build first. This is the form the
// cascade uses and the one the other is written in terms of: a declaration that
// has been through `var()` substitution has a value that is *not* the one on
// the rule, so applying it through the struct meant copying the property name
// and the substituted value into a temporary, per declaration, per element. The
// views must outlive the call, which they trivially do -- the rule owns one and
// the substitution buffer owns the other.
bool ApplyDeclaration(std::string_view property, std::string_view value,
                      const ComputedStyle& parent, ComputedStyle& style,
                      const MediaContext& context = {});

// Whether this engine supports `property: value` -- the question `@supports`
// asks, answered by trying it.
//
// A custom property is always supported; it has no grammar to fail. Anything
// else is applied to a scratch style, and the answer is whether it took. A
// table of supported names maintained beside the implementation would be the
// obvious alternative and is the trap ADR 0014 §3 names: it starts correct and
// then a property is added without it, and the page is told no about something
// that works -- or, worse, yes about something that does not.
bool SupportsDeclaration(std::string_view property, std::string_view value,
                         const MediaContext& context = {});

}  // namespace microbrowser::css
