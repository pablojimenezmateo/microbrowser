#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dom/FlatTree.h"
#include "text/Normalize.h"
#include "text/UnicodeProperties.h"
#include "html/FormControl.h"
#include "layout/LayoutEngine.h"
#include "layout/ReplacedBoxes.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

// Building the box tree: which DOM nodes get boxes, of what kind, and where the boxes CSS invents
// but the document never wrote come from.
//
// Split from LayoutEngine.cpp when that file went over the module's line cap, and the cap was
// pointing at the seam the header already names: building the tree and laying it out are two
// phases, and only the first one needs the DOM. Everything here is a question about *what the
// boxes are* -- display, replacedness, anonymous wrapping, whitespace between siblings. Nothing
// here has a geometry in it.

namespace microbrowser::layout {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsAllWhitespace(std::string_view text) {
  return std::all_of(text.begin(), text.end(), IsSpace);
}

// A text box that is nothing but collapsible whitespace. Between two blocks it generates nothing --
// keeping it would put a blank line between every pair of paragraphs -- but between two inlines it
// is the space between two words, and dropping it renders "boldand italic".
// `text-transform`, applied to the text a box will hold.
//
// **The case mapping is ASCII and Latin-1 only, and that is a deliberate stop rather than an
// oversight.** A correct one is Unicode's SpecialCasing plus the Turkish and Lithuanian tailorings
// -- one-to-many mappings and a language the cascade does not carry -- and `src/text` has no case
// table today. What is here is the mapping that is *unambiguous*: a wrong uppercase is worse than
// an untouched one, so anything outside these two blocks is left alone.
std::uint32_t TransformedCodePoint(std::uint32_t c, bool upper) {
  if (c < 0x80) {
    if (upper && c >= 'a' && c <= 'z') return c - 32;
    if (!upper && c >= 'A' && c <= 'Z') return c + 32;
    return c;
  }
  // Latin-1 supplement: the two case ranges, minus the three code points that are not a pair --
  // U+00D7 MULTIPLICATION SIGN, U+00F7 DIVISION SIGN and U+00DF SHARP S, whose uppercase is "SS".
  if (upper && c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) return c - 0x20;
  if (!upper && c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 0x20;
  return c;
}

// The fullwidth forms: ASCII punctuation and letters map into U+FF01..U+FF5E, and the space to the
// ideographic space. A one-to-one mapping over one contiguous block, so it is a rule rather than a
// table.
std::uint32_t FullWidthCodePoint(std::uint32_t c) {
  if (c >= 0x21 && c <= 0x7E) return c + 0xFEE0;
  if (c == 0x20) return 0x3000;
  return c;
}

bool IsWordSeparator(std::uint32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == 0x00A0;
}

std::string ApplyTextTransform(std::string_view text, const css::TextTransform& transform) {
  if (transform.IsNone() || text.empty()) {
    return std::string(text);
  }
  const text::CodePoints input = text::DecodeUtf8(text);
  std::string out;
  out.reserve(text.size());
  bool at_word_start = true;
  for (const std::uint32_t original : input) {
    std::uint32_t c = original;
    switch (transform.letter_case) {
      case css::TextCase::None:
        break;
      case css::TextCase::Uppercase:
        c = TransformedCodePoint(c, true);
        break;
      case css::TextCase::Lowercase:
        c = TransformedCodePoint(c, false);
        break;
      case css::TextCase::Capitalize:
        // The first *typographic letter unit* of each word. Word boundaries are decided within
        // this text node, which is where this browser can see them: `<span>a</span>b` is one word
        // to the specification and two text boxes here, and the second would be capitalized. The
        // alternative is a pass over the finished line, which is a different design.
        if (at_word_start) {
          c = TransformedCodePoint(c, true);
        }
        break;
    }
    if (transform.full_width) {
      c = FullWidthCodePoint(c);
    }
    at_word_start = IsWordSeparator(original);
    text::AppendUtf8(out, c);
  }
  return out;
}

bool IsCollapsibleSpace(const Box& box) {
  return box.GetKind() == Box::Kind::Text &&
         box.Style().white_space_collapse == css::WhiteSpaceCollapse::Collapse &&
         IsAllWhitespace(box.Text());
}

// Which bytes of `text` the `::first-letter` pseudo-element covers, as a half-open range. Empty
// when this run has no first letter in it at all.
//
// CSS 2.1 s5.12.2: the first letter, plus any punctuation that *precedes or follows* it -- so
// `")T)est"` has a first letter of `")T)"` and `"...est"` has none, because a run that is only
// punctuation never reaches a letter and the pseudo continues onto whatever comes next.
//
// The range has a `begin` rather than being a length because leading white space is skipped and
// **left out**, so a background declared on the pseudo does not paint behind a space. The test that
// names the question is `css/css-backgrounds/first-letter-space-not-selected.html`, and it asks for
// more than this: it wants the pseudo to match nothing at all in `&nbsp;A`. **Firefox fails it too**
// and so do we, and no test in the checkout distinguishes skipping the space from taking it as the
// letter -- measured, both ways, at 8,355 of 20,998 reftests. Skipping is kept because it is the
// reading that puts the emphasis on the letter rather than on an invisible character. The predicate
// is `text::IsSpaceSeparator` rather than `IsSpace` because the four characters that test uses are
// precisely the ones whitespace collapsing leaves alone, so they are the only ones that reach here.
//
// "One letter" is one code point rather than one grapheme cluster. The difference is a base
// character followed by combining marks, which this would split; the marks are folded in below by
// the same loop that takes the trailing punctuation, because a combining mark is not punctuation
// and would end it -- so the accents ride along with the letter and the cluster survives. What is
// left unhandled is a digraph a language treats as one letter (Dutch `ij`), which no engine gets
// right without a locale.
struct FirstLetterRange {
  std::size_t begin = 0;
  std::size_t end = 0;

  bool Empty() const { return begin == end; }
};

FirstLetterRange FirstLetterExtent(std::string_view text) {
  const auto take_while = [&text](std::size_t& cursor, bool (*predicate)(std::uint32_t)) {
    while (cursor < text.size()) {
      std::size_t next = cursor;
      std::uint32_t code = 0;
      if (!util::DecodeUtf8(text, next, code) || !predicate(code)) {
        return;
      }
      cursor = next;
    }
  };
  std::size_t at = 0;
  take_while(at, [](std::uint32_t code) {
    return code == '\t' || code == '\n' || code == '\r' || code == '\f' ||
           text::IsSpaceSeparator(code);
  });
  const std::size_t begin = at;
  take_while(at, text::IsFirstLetterPunctuation);
  // The letter itself. A run that reached the end without one -- all punctuation, all space, or
  // empty -- has no first letter, and answering with the punctuation alone would style it and leave
  // the letter it belongs to plain.
  std::size_t after_letter = at;
  std::uint32_t letter = 0;
  if (!util::DecodeUtf8(text, after_letter, letter)) {
    return FirstLetterRange{};
  }
  at = after_letter;
  take_while(at, text::IsFirstLetterPunctuation);
  return FirstLetterRange{begin, at};
}

// Splits the leading characters of the first in-flow text in `children` into an inline box of their
// own, styled by `first_letter`. CSS 2.1 s5.12.2's `::first-letter`.
//
// True when a split happened. The walk descends through inline boxes -- `<p><em>Once</em>...` has
// its first letter inside the `<em>` -- and stops at anything else, because a block, a replaced
// element or an atomic inline is content before the text and the specification's condition is that
// the letter is not preceded by any.
bool SplitFirstLetter(std::vector<std::unique_ptr<Box>>& children,
                      const css::ComputedStyle& first_letter) {
  for (std::unique_ptr<Box>& child : children) {
    if (child->IsOutOfLineFlow()) {
      continue;  // a float or an absolutely positioned box is not on this line
    }
    if (child->GetKind() == Box::Kind::Inline) {
      if (SplitFirstLetter(child->MutableChildren(), first_letter)) {
        return true;
      }
      continue;
    }
    if (child->GetKind() != Box::Kind::Text) {
      return false;
    }
    const std::string& text = child->Text();
    const FirstLetterRange letter = FirstLetterExtent(text);
    if (letter.Empty()) {
      // No letter in this run. Only white space may be skipped past: anything else is content
      // before the first letter, which the pseudo does not reach across.
      if (IsAllWhitespace(text)) {
        continue;
      }
      return false;
    }
    // The same boxes the suite's own references write by hand: a `<span>` around the letter, with
    // whatever came before it and whatever comes after beside it. An inline box rather than a text
    // box with a second style, because `::first-letter` takes margins, padding and a border, and
    // only a box has those.
    auto letter_box = std::make_unique<Box>(Box::Kind::Inline, first_letter);
    auto letter_text = std::make_unique<Box>(Box::Kind::Text, TextStyleFrom(first_letter));
    letter_text->SetText(text.substr(letter.begin, letter.end - letter.begin));
    letter_box->Append(std::move(letter_text));

    std::vector<std::unique_ptr<Box>> replacement;
    if (letter.begin > 0) {
      // Leading space, in a text box of the *originating* element's style rather than the pseudo's:
      // it is not in the pseudo, so a background declared there must not paint behind it.
      auto leading = std::make_unique<Box>(Box::Kind::Text, child->Style());
      leading->SetText(text.substr(0, letter.begin));
      replacement.push_back(std::move(leading));
    }
    replacement.push_back(std::move(letter_box));
    if (letter.end < text.size()) {
      child->SetText(text.substr(letter.end));
      replacement.push_back(std::move(child));
    }
    // Splices in place of the box being iterated, and the loop is left immediately -- the insertion
    // invalidates every iterator into `children`.
    const auto at = static_cast<std::ptrdiff_t>(&child - children.data());
    const auto begin = children.begin() + at;
    children.erase(begin);
    children.insert(children.begin() + at, std::make_move_iterator(replacement.begin()),
                    std::make_move_iterator(replacement.end()));
    return true;
  }
  return false;
}

}  // namespace

std::unique_ptr<Box> LayoutEngine::BuildFor(const dom::Node& node,
                                            const css::ComputedStyle& parent_style,
                                            std::uint64_t parent_style_id,
                                            bool& produced_inline) const {
  if (node.IsText()) {
    const auto& text_node = static_cast<const dom::Text&>(node);
    // Three whitespace-processing modes rather than two. `preserve-breaks` (`pre-line`) is the
    // one that was missing: it collapses spaces like `normal` and keeps segment breaks like `pre`,
    // and folding it into either of the other two loses exactly the thing it is for.
    const css::WhiteSpaceCollapse collapse = parent_style.white_space_collapse;
    std::string text = css::PreservesSpaces(collapse) ? text_node.Data()
                       : css::PreservesNewlines(collapse)
                           ? CollapseWhitespaceKeepingBreaks(text_node.Data())
                           : CollapseWhitespace(text_node.Data());
    // After whitespace processing, because `text-transform: full-width` turns a space into an
    // ideographic space -- which is not collapsible, and would be collapsed if the order were the
    // other way round.
    text = ApplyTextTransform(text, parent_style.text_transform);
    if (text.empty()) {
      return nullptr;
    }
    auto box = std::make_unique<Box>(Box::Kind::Text, TextStyleFrom(parent_style));
    box->SetText(std::move(text));
    produced_inline = true;
    return box;
  }

  if (!node.IsElement()) {
    return nullptr;  // comments and doctypes generate no boxes
  }

  const auto& element = static_cast<const dom::Element&>(node);
  std::uint64_t style_id = 0;
  const css::ComputedStyle style =
      resolver_->StyleFor(element, parent_style, parent_style_id, &style_id);
  if (!style.GeneratesBox()) {
    return nullptr;
  }

  if (html::IsHiddenInput(element)) {
    return nullptr;
  }

  if (element.TagName() == "br") {
    auto box = std::make_unique<Box>(Box::Kind::LineBreak, style);
    box->SetOrigin(&element);
    produced_inline = true;
    return box;
  }

  // A replaced element's children generate no boxes: whatever is inside an
  // <img> is fallback content the element replaces, and form controls have
  // their own control surface rather than ordinary DOM child boxes.
  // Every box may carry one, replaced or not, so it is resolved before the
  // kinds diverge rather than in each branch.
  const auto attach_background = [this, &style](Box& box) {
    if (style.background.image.empty() || images_ == nullptr) {
      return;
    }
    images_->WantImage(style.background.image);
    box.SetBackgroundImage(images_->ImageFor(style.background.image));
  };

  if (IsReplacedElement(element)) {
    auto box = std::make_unique<Box>(Box::Kind::Replaced, style);
    box->SetOrigin(&element);
    attach_background(*box);
    // `<img>` / `<canvas>`: attach pixels *before* measuring. Intrinsic size
    // comes from the bitmap; measuring first left every undeclared `<img>` at
    // 0×0 (`57418b6`, which moved SetImage after measure for SVG raster size).
    // Inline `<svg>` still needs a CSS size to rasterize against, so it
    // measures declared size first, then attaches, then remeasures.
    if ((element.TagName() == "img" || element.TagName() == "canvas") && images_ != nullptr) {
      box->SetImage(images_->ImageForElement(element, 0, 0));
    } else if (element.TagName() == "video" && images_ != nullptr) {
      if (const std::optional<gfx::SurfaceId> surface = images_->SurfaceForElement(element)) {
        box->SetVideoSurface(*surface);
      }
    } else if (element.TagName() == "input" || element.TagName() == "textarea" ||
               element.TagName() == "select") {
      box->SetText(FormControlText(element));
    }
    float width = ReplacedWidth(*box);
    float height = ReplacedHeight(*box);
    if (element.TagName() == "svg" && images_ != nullptr) {
      box->SetImage(images_->ImageForElement(element, static_cast<int>(width + 0.5f),
                                             static_cast<int>(height + 0.5f)));
      width = ReplacedWidth(*box);
      height = ReplacedHeight(*box);
    }
    box->Geometry().content = gfx::FloatRect{0.0f, 0.0f, width, height};
    // Not unconditionally inline: the box answers with its own display, so
    // `img { display: block }` puts the picture on a line of its own and a
    // floated or absolutely positioned one leaves the flow. See
    // Box::IsBlockLevelReplaced.
    produced_inline = box->IsInlineLevel();
    return box;
  }

  // Children are gathered before this box is created, because two things about
  // it are not knowable until they exist: whether it mixes inline and block
  // content, and -- for a declared inline -- whether it contains a block at all.
  const bool declared_inline = style.IsInlineLevel();
  std::vector<std::unique_ptr<Box>> children;
  bool any_inline = false;
  bool any_block = false;

  const auto append_generated = [&](css::PseudoElement which, bool prepend) {
    const css::ComputedStyle pseudo = resolver_->StyleForPseudo(element, which, style);
    const bool generates = pseudo.content == css::ComputedStyle::Content::Empty ||
                           pseudo.content == css::ComputedStyle::Content::String;
    // A column box renders nothing -- CSS 2.1 s17.2, and the four
    // `*-content-display-01{2,3}.xht` files state it in their own `assert`: generated content whose
    // `display` is `table-column` or `table-column-group` is "not rendered (exactly as if they had
    // display: none)". Not rendered rather than empty: the background goes too, which is why this
    // is a refusal to make the box and not a refusal to fill it. The rule is narrower here than in
    // the specification on purpose -- a real `<col>` does paint a column background, and that is a
    // table question rather than a generated-content one.
    const bool column_box = pseudo.display == css::Display::TableColumn ||
                            pseudo.display == css::Display::TableColumnGroup;
    if (!generates || !pseudo.GeneratesBox() || column_box) {
      return;
    }
    const bool atomic = pseudo.IsAtomicInline();
    const bool inline_level = atomic || pseudo.IsInlineLevel();
    const Box::Kind kind = atomic         ? Box::Kind::InlineBlock
                           : inline_level ? Box::Kind::Inline
                                          : Box::Kind::Block;
    auto generated = std::make_unique<Box>(kind, pseudo);
    // The text of a string `content`, as an ordinary text child -- the specification's own model,
    // "as if it were a real element inserted just inside its associated element", so a `display`
    // declared on the pseudo decides the box and the text simply sits in it. It goes through the
    // same whitespace processing and `text-transform` as document text, because `content: "  a  "`
    // collapses exactly like the markup it stands for.
    if (pseudo.content == css::ComputedStyle::Content::String) {
      const css::WhiteSpaceCollapse collapse = pseudo.white_space_collapse;
      std::string text = css::PreservesSpaces(collapse) ? pseudo.content_text
                         : css::PreservesNewlines(collapse)
                             ? CollapseWhitespaceKeepingBreaks(pseudo.content_text)
                             : CollapseWhitespace(pseudo.content_text);
      text = ApplyTextTransform(text, pseudo.text_transform);
      if (!text.empty()) {
        auto content_text = std::make_unique<Box>(Box::Kind::Text, TextStyleFrom(pseudo));
        content_text->SetText(std::move(text));
        // A flex container's children are flex items, and a run of text is one anonymous
        // block-container item (CSS Flexbox s4) -- the same wrapping the element path does below
        // for a declared flex container. It has to be repeated here because a generated box is
        // built in this lambda rather than through `BuildFor`, so it reaches none of that code.
        // Without it `::after { content: "x"; display: flex }` lays its text out as a bare child of
        // a flex container and renders differently from the same declaration with `display: block`,
        // which is exactly what `css/css-flexbox/flexbox_generated-flex.html` compares.
        if (pseudo.IsFlexContainer()) {
          css::ComputedStyle item = css::StyleResolver::InitialStyle();
          css::InheritInto(pseudo, item);
          item.display = css::Display::Block;
          auto anonymous = std::make_unique<Box>(Box::Kind::AnonymousBlock, item);
          anonymous->Append(std::move(content_text));
          generated->Append(std::move(anonymous));
        } else {
          generated->Append(std::move(content_text));
        }
      }
    }
    // No Origin: a generated box is not a DOM node, and hit-testing /
    // script geometry must not pretend otherwise.
    any_inline = any_inline || inline_level;
    any_block = any_block || generated->IsBlockLevel();
    if (prepend) {
      children.insert(children.begin(), std::move(generated));
    } else {
      children.push_back(std::move(generated));
    }
  };

  append_generated(css::PseudoElement::Before, true);
  // The flattened tree, not the node tree: ADR 0019 §2.
  for (dom::Node* child : dom::FlatChildren(node)) {
    bool child_inline = false;
    std::unique_ptr<Box> child_box = BuildFor(*child, style, style_id, child_inline);
    if (child_box == nullptr) {
      continue;
    }
    any_inline = any_inline || child_inline;
    any_block = any_block || child_box->IsBlockLevel();
    children.push_back(std::move(child_box));
  }
  append_generated(css::PseudoElement::After, false);

  // Whitespace between two blocks generates no box -- keeping it would put a
  // blank line between every pair of paragraphs -- but whitespace between two
  // *inlines* is the space between two words, and dropping it renders
  // "boldand italic". The difference is what the neighbours are, which is only
  // knowable here, after they have all been built.
  if (style.white_space_collapse == css::WhiteSpaceCollapse::Collapse) {
    std::vector<std::unique_ptr<Box>> kept;
    kept.reserve(children.size());
    for (std::size_t i = 0; i < children.size(); ++i) {
      if (!IsCollapsibleSpace(*children[i])) {
        kept.push_back(std::move(children[i]));
        continue;
      }
      // Dropped at the edges of the block too: a line never begins or ends
      // with a collapsible space.
      // The previous sibling is read from `kept`, not from `children`: the ones
      // already kept were moved out and left null behind them.
      const bool inline_before = !kept.empty() && !kept.back()->IsOutOfLineFlow();
      const bool inline_after =
          i + 1 < children.size() && !children[i + 1]->IsOutOfLineFlow();
      if (inline_before && inline_after) {
        kept.push_back(std::move(children[i]));
      }
    }
    children = std::move(kept);
    any_inline = false;
    any_block = false;
    for (const std::unique_ptr<Box>& child : children) {
      any_inline = any_inline || !child->IsOutOfLineFlow();
      any_block = any_block || child->IsOutOfLineFlow();
    }
  }

  // An inline box containing a block is not an inline box. CSS 2.1 s9.2.1.1
  // splits the inline around the block and wraps each part in an anonymous
  // block; this promotes the whole element instead, which produces the same
  // boxes for the case the web actually writes -- `<a><div>...</div></a>`,
  // which is how Hacker News draws its vote arrows and how most of the web
  // makes a card clickable.
  //
  // Left inline, the block child is never laid out at all: line layout walks
  // *through* a non-inline child collecting the inline content inside it, so a
  // block with no inline content simply vanishes, taking its background and its
  // borders with it. That is a wrong render with no error, which is the worst
  // kind.
  // An atomic inline is exempt from the promotion above, and that is the whole
  // difference between it and an inline box: `inline-block` is *defined* as
  // "block on the inside", so a block child is what it is for rather than a
  // contradiction to resolve. Promoting it would take it off the line it
  // belongs on.
  const bool atomic = style.IsAtomicInline();
  const bool inline_level = atomic || (declared_inline && !any_block);
  const Box::Kind kind = atomic          ? Box::Kind::InlineBlock
                         : inline_level  ? Box::Kind::Inline
                                         : Box::Kind::Block;
  // CSS 2.1 s5.12.2. A block container only -- an inline box's first letter belongs to the block
  // that contains it -- and only when some rule actually said `::first-letter`, which
  // `AnyRuleTargets` answers from a flag rather than from a cascade walk per block.
  //
  // **Not a flex or grid container.** CSS Grid s3 and CSS Flexbox s4 both say the pseudo does not
  // apply to one, and the reason is the box tree rather than the selector: such a container's
  // children are items, and splitting the first of them in two would make two items out of one.
  // `css/css-grid/grid-model/grid-container-ignores-first-letter-001.html` is the file that says so
  // -- it puts `line-height: 100px` on the pseudo and asserts the item is still 20px tall.
  //
  // The grid half of the test is unreachable today: `ApplyDeclaration` refuses `display: grid` so
  // that `@supports (display: grid)` answers an honest false, so a grid container is a block and
  // takes the flex clause's fate only when someone writes `display: flex`. It is stated for both
  // anyway, because the day the display value lands is not the day anyone will think of this.
  const bool item_container = style.IsFlexContainer() || style.display == css::Display::Grid ||
                              style.display == css::Display::InlineGrid;
  if (kind != Box::Kind::Inline && !item_container &&
      resolver_->AnyRuleTargets(css::PseudoElement::FirstLetter)) {
    bool matched = false;
    const css::ComputedStyle first_letter = resolver_->StyleForPseudo(
        element, css::PseudoElement::FirstLetter, style, &matched);
    if (matched) {
      SplitFirstLetter(children, first_letter);
    }
  }

  auto box = std::make_unique<Box>(kind, style);
  box->SetOrigin(&element);
  attach_background(*box);
  produced_inline = inline_level;

  // An anonymous box has no declarations of its own: it inherits, and every other property is at
  // its initial value (CSS 2.1 §9.2.1.1). Copying the parent's whole style instead gave it the
  // parent's `width`, `padding`, `background` and -- worst -- its `display`, so an anonymous child
  // of a flex container was itself a flex container 1280px wide. That is what the flex branch
  // below used to be avoiding by refusing to wrap at all.
  const auto anonymous_style = [&style] {
    css::ComputedStyle anonymous = css::StyleResolver::InitialStyle();
    css::InheritInto(style, anonymous);
    anonymous.display = css::Display::Block;
    return anonymous;
  };

  // Block containers wrap mixed inline/block runs in anonymous blocks. A flex container wraps
  // something narrower: each contiguous run of child *text* is one anonymous block-container flex
  // item, and an element child is already a flex item of its own (CSS Flexbox §4). Gathering the
  // elements too would put a whole row of chips into one item.
  const bool wrap_inline_runs = !style.IsFlexContainer() && any_inline && any_block;
  const bool wrap_flex_text = style.IsFlexContainer() && kind != Box::Kind::Inline;
  // `kind != Inline` rather than `!inline_level`: an atomic inline is
  // inline-level on the outside and a block container on the inside, so it
  // needs the anonymous wrapping that a block container needs and an inline box
  // does not.
  if (wrap_flex_text) {
    // A run of text with nothing but white space in it is not rendered at all -- CSS Flexbox §4
    // says so explicitly, and without the rule every gap between two chips in the markup would
    // become an empty flex item taking a share of the main size.
    std::unique_ptr<Box> pending;
    const auto flush = [&] {
      if (pending == nullptr) {
        return;
      }
      bool any_ink = false;
      for (const std::unique_ptr<Box>& text : pending->Children()) {
        any_ink = any_ink || !IsCollapsibleSpace(*text);
      }
      if (any_ink) {
        box->Append(std::move(pending));
      }
      pending.reset();
    };
    for (std::unique_ptr<Box>& child : children) {
      const bool is_text =
          child->GetKind() == Box::Kind::Text || child->GetKind() == Box::Kind::LineBreak;
      if (!is_text) {
        flush();
        box->Append(std::move(child));
        continue;
      }
      if (pending == nullptr) {
        pending = std::make_unique<Box>(Box::Kind::AnonymousBlock, anonymous_style());
      }
      pending->Append(std::move(child));
    }
    flush();
  } else if (kind != Box::Kind::Inline && wrap_inline_runs) {
    // Mixed content. Consecutive inline children are wrapped in anonymous
    // blocks, which is the only way the two kinds can be siblings — a block
    // formatting context contains blocks, and inline content needs one of its
    // own.
    std::unique_ptr<Box> pending;
    for (std::unique_ptr<Box>& child : children) {
      if (child->IsOutOfLineFlow()) {
        if (pending != nullptr) {
          box->Append(std::move(pending));
        }
        box->Append(std::move(child));
        continue;
      }
      if (pending == nullptr) {
        pending = std::make_unique<Box>(Box::Kind::AnonymousBlock, anonymous_style());
      }
      pending->Append(std::move(child));
    }
    if (pending != nullptr) {
      box->Append(std::move(pending));
    }
  } else {
    for (std::unique_ptr<Box>& child : children) {
      box->Append(std::move(child));
    }
  }
  return box;
}

std::unique_ptr<Box> LayoutEngine::BuildBoxTree(const dom::Document& document) const {
  AddPerformanceCounter(PerfCounterId::LayoutTreeBuilds);
  css::ComputedStyle root_style = css::StyleResolver::InitialStyle();
  root_style.display = css::Display::Block;

  auto root = std::make_unique<Box>(Box::Kind::Block, root_style);
  for (dom::Node* child : dom::FlatChildren(document)) {
    bool produced_inline = false;
    if (std::unique_ptr<Box> box = BuildFor(*child, root_style, 0, produced_inline)) {
      root->Append(std::move(box));
    }
  }
  return root;
}

}  // namespace microbrowser::layout
