#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "dom/FlatTree.h"
#include "html/FormControl.h"
#include "layout/LayoutEngine.h"
#include "layout/ReplacedBoxes.h"
#include "util/PerformanceCounters.h"

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
bool IsCollapsibleSpace(const Box& box) {
  return box.GetKind() == Box::Kind::Text &&
         box.Style().white_space == css::WhiteSpace::Normal && IsAllWhitespace(box.Text());
}

}  // namespace

std::unique_ptr<Box> LayoutEngine::BuildFor(const dom::Node& node,
                                            const css::ComputedStyle& parent_style,
                                            std::uint64_t parent_style_id,
                                            bool& produced_inline) const {
  if (node.IsText()) {
    const auto& text_node = static_cast<const dom::Text&>(node);
    std::string text = parent_style.white_space == css::WhiteSpace::Pre ||
                               parent_style.white_space == css::WhiteSpace::PreWrap
                           ? text_node.Data()
                           : CollapseWhitespace(text_node.Data());
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
    if (pseudo.content != css::ComputedStyle::Content::Empty || !pseudo.GeneratesBox()) {
      return;
    }
    const bool atomic = pseudo.IsAtomicInline();
    const bool inline_level = atomic || pseudo.IsInlineLevel();
    const Box::Kind kind = atomic         ? Box::Kind::InlineBlock
                           : inline_level ? Box::Kind::Inline
                                          : Box::Kind::Block;
    auto generated = std::make_unique<Box>(kind, pseudo);
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
  if (style.white_space == css::WhiteSpace::Normal) {
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
