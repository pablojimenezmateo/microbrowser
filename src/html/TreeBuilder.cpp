#include "html/TreeBuilder.h"

#include <algorithm>
#include <array>

#include "html/TreeBuilderInternal.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// Elements that close an open `<p>` when they start. The list is in the spec
// and is not derivable from anything — block-level-ness is not a property the
// parser can compute.
constexpr std::array<std::string_view, 25> kClosesParagraph = {
    "address", "article", "aside",  "blockquote", "details", "div",   "dl",     "fieldset",
    "figure",  "footer",  "form",   "h1",         "h2",      "h3",    "h4",     "h5",
    "h6",      "header",  "hr",     "main",       "menu",    "nav",   "ol",     "p",
    "ul"};

// Tags whose presence means the token belongs to an insertion mode this builder
// does not implement. Counted rather than mishandled.
constexpr std::array<std::string_view, 1> kUnsupportedTags = {"frameset"};

// Elements the spec ends implicitly when a parent closes: `<li>` does not need
// `</li>`, and neither do table cells or definition list items.
constexpr std::array<std::string_view, 8> kImpliedEndTags = {
    "dd", "dt", "li", "optgroup", "option", "p", "rp", "rt"};

// The wider list the spec calls "thoroughly", which also ends the table
// elements. Only `</template>` generates these, and only because a template may
// legally be closed with a row still open inside it.
constexpr std::array<std::string_view, 15> kThoroughEndTags = {
    "caption", "colgroup", "dd",  "dt",    "li",    "optgroup", "option", "p",
    "rp",      "rt",       "tbody", "td",  "tfoot", "th",       "thead"};

// §13.2.4.2. The elements that stop a scope walk. The foreign-content roots
// belong here too and are absent because foreign content is.
constexpr std::array<std::string_view, 9> kScopeStoppers = {
    "applet", "caption", "html", "marquee", "object", "table", "td", "template", "th"};

// The head's own element set, which "in head" and "in template" both delegate
// to. Void ones: they never become the current node.
constexpr std::array<std::string_view, 5> kHeadVoidTags = {"base", "basefont", "bgsound", "link",
                                                           "meta"};

// §13.2.6.1, "the appropriate place for inserting a node": normally the current
// node, but inside a table with foster parenting on, immediately before the
// table. `before` is null for an append.
struct InsertionPoint {
  dom::Node* parent = nullptr;
  const dom::Node* before = nullptr;
};

// A template's contents rather than the template, for any element that has
// them. §13.2.6.1 step 2.1: everything the parser inserts into a `<template>`
// goes into the separate fragment, which is what keeps the markup inside one
// out of the document it appeared in.
dom::Node& InsertionTarget(dom::Node& node) {
  if (!node.IsElement()) {
    return node;
  }
  dom::DocumentFragment* content = static_cast<dom::Element&>(node).Content();
  return content == nullptr ? node : static_cast<dom::Node&>(*content);
}

// §13.2.6.1, "the appropriate place for inserting a node". A free function
// rather than a member because its return type is local to this file, and the
// header should not have to name it.
InsertionPoint AppropriatePlace(const std::vector<dom::Element*>& stack, bool foster_parenting,
                                dom::Document& document) {
  dom::Node* current =
      stack.empty() ? static_cast<dom::Node*>(&document) : static_cast<dom::Node*>(stack.back());
  if (!foster_parenting || stack.empty() || !IsFosterParent(stack.back()->TagName())) {
    return {&InsertionTarget(*current), nullptr};
  }
  for (std::size_t i = stack.size(); i-- > 0;) {
    dom::Element* table = stack[i];
    // A template above the last table takes the node instead: content inside a
    // template is never fostered out of it, because the template's contents are
    // not in any document to be fostered into.
    if (table->Content() != nullptr) {
      return {&InsertionTarget(*table), nullptr};
    }
    if (table->TagName() != "table") {
      continue;
    }
    if (table->Parent() != nullptr) {
      return {table->Parent(), table};
    }
    // A table that is not in the tree cannot be inserted before. The spec's
    // fallback is the element below it on the stack.
    return {i > 0 ? &InsertionTarget(*stack[i - 1]) : &InsertionTarget(*current), nullptr};
  }
  return {&InsertionTarget(*stack.front()), nullptr};
}

// The node that will precede an insertion at this point, or null. Text runs
// merge into it, so it has to be found the same way for both cases.
dom::Node* NodeBefore(const InsertionPoint& at) {
  if (at.before == nullptr) {
    return at.parent->LastChild();
  }
  dom::Node* previous = nullptr;
  for (const std::unique_ptr<dom::Node>& child : at.parent->Children()) {
    if (child.get() == at.before) {
      return previous;
    }
    previous = child.get();
  }
  return nullptr;
}

}  // namespace

dom::Node& TreeBuilder::CurrentNode() {
  if (open_elements_.empty()) {
    return *document_;
  }
  return *open_elements_.back();
}

bool TreeBuilder::HasInScope(std::string_view tag_name, Scope scope) const {
  for (std::size_t i = open_elements_.size(); i-- > 0;) {
    const std::string& name = open_elements_[i]->TagName();
    if (name == tag_name) {
      return true;
    }
    switch (scope) {
      case Scope::Table:
        // The narrowest walk: a `</tr>` must not reach past its own table into
        // an outer one, which is how nested tables stay separate.
        if (name == "html" || name == "table") {
          return false;
        }
        continue;
      case Scope::Select:
        if (name != "optgroup" && name != "option") {
          return false;
        }
        continue;
      case Scope::Button:
        if (name == "button") {
          return false;
        }
        break;
      case Scope::ListItem:
        if (name == "ol" || name == "ul") {
          return false;
        }
        break;
      case Scope::Default:
        break;
    }
    if (Contains(kScopeStoppers, name)) {
      return false;
    }
  }
  return false;
}

void TreeBuilder::PopCurrent() {
  if (open_elements_.size() > stack_floor_) {
    open_elements_.pop_back();
  }
}

void TreeBuilder::PopUntil(std::string_view tag_name) {
  while (open_elements_.size() > stack_floor_) {
    const bool matched = open_elements_.back()->TagName() == tag_name;
    open_elements_.pop_back();
    if (matched) {
      return;
    }
  }
}

void TreeBuilder::ClearStackToContext(std::initializer_list<std::string_view> context) {
  while (open_elements_.size() > stack_floor_) {
    const std::string& name = open_elements_.back()->TagName();
    if (name == "html" || name == "template" || Contains(context, name)) {
      return;
    }
    open_elements_.pop_back();
  }
}

void TreeBuilder::GenerateImpliedEndTags(std::string_view except, bool thoroughly) {
  while (open_elements_.size() > stack_floor_) {
    const std::string& name = open_elements_.back()->TagName();
    const bool implied =
        thoroughly ? Contains(kThoroughEndTags, name) : Contains(kImpliedEndTags, name);
    if (name == except || !implied) {
      return;
    }
    open_elements_.pop_back();
  }
}

dom::Element& TreeBuilder::InsertElement(const Token& token) {
  auto element = std::make_unique<dom::Element>(token.data);
  for (const Attribute& attribute : token.attributes) {
    element->SetAttribute(attribute.name, attribute.value);
  }
  dom::Element* raw = element.get();
  const InsertionPoint at = AppropriatePlace(open_elements_, foster_parenting_, *document_);
  at.parent->InsertBefore(std::move(element), at.before);
  // A void element never becomes the current node, or the rest of the document
  // would nest inside it.
  //
  // A *trailing solidus* on anything else does not close it. `<tr/>` is `<tr>`,
  // not `<tr></tr>`: HTML acknowledges the self-closing flag only on void
  // elements and on foreign content, and this parser has no foreign content.
  // Honoring it here reads as a fix and is a bug -- Hacker News writes
  // `<tr style='height:10px'/>` between its rows, and closing that row early
  // left the next `<tr>` with no row to close, so every row after it was
  // foster-parented out of the table and the page fell apart below the header.
  if (!dom::IsVoidElement(token.data)) {
    if (token.self_closing) {
      ++errors_;
    }
    open_elements_.push_back(raw);
  }
  return *raw;
}

void TreeBuilder::InsertText(std::string_view text) {
  if (text.empty()) {
    return;
  }
  const InsertionPoint at = AppropriatePlace(open_elements_, foster_parenting_, *document_);
  // Appended to the previous text node when there is one, so that a run split
  // across tokens is one node. A DOM with adjacent text nodes is observably
  // different from one without.
  dom::Node* previous = NodeBefore(at);
  if (previous != nullptr && previous->IsText()) {
    static_cast<dom::Text*>(previous)->Append(text);
    return;
  }
  at.parent->InsertBefore(std::make_unique<dom::Text>(std::string(text)), at.before);
}

dom::Element& TreeBuilder::InsertImplied(std::string_view tag_name) {
  Token implied;
  implied.kind = Token::Kind::StartTag;
  implied.data = std::string(tag_name);
  return InsertElement(implied);
}

void TreeBuilder::InsertComment(const std::string& data, dom::Node* parent) {
  (parent != nullptr ? *parent : CurrentNode()).Append(std::make_unique<dom::Comment>(data));
}

void TreeBuilder::SwitchToRawText(const Token& token, TokenizerState state) {
  InsertElement(token);
  tokenizer_.SetLastStartTag(token.data);
  tokenizer_.SwitchTo(state);
  original_mode_ = mode_;
  mode_ = InsertionMode::Text;
}

void TreeBuilder::ProcessInBody(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
      InsertText(token.data);
      if (!IsWhitespaceOnly(token.data)) {
        frameset_ok_ = false;
      }
      return;

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;  // a doctype here is a parse error and is ignored
      return;

    case Token::Kind::StartTag: {
      if (Contains(kUnsupportedTags, token.data)) {
        ++unsupported_;
        AddPerformanceCounter(PerfCounterId::HtmlUnsupportedInsertionMode);
        return;
      }
      if (token.data == "html") {
        ++errors_;
        return;
      }
      if (token.data == "body" || token.data == "head") {
        ++errors_;
        return;
      }
      if (token.data == "title") {
        SwitchToRawText(token, TokenizerState::RcData);
        return;
      }
      if (token.data == "table") {
        // A table does not close an open paragraph in quirks mode. Real pages
        // depend on both halves of that.
        if (!document_->InQuirksMode() && HasInScope("p", Scope::Button)) {
          PopUntil("p");
        }
        InsertElement(token);
        frameset_ok_ = false;
        mode_ = InsertionMode::InTable;
        return;
      }
      if (token.data == "select") {
        InsertElement(token);
        frameset_ok_ = false;
        ResetInsertionMode();
        return;
      }
      if (Contains(kTableStructureTags, token.data)) {
        // Table structure outside a table: a parse error, and dropped. The
        // table modes are where these are meaningful.
        ++errors_;
        return;
      }
      if (token.data == "script" || token.data == "style" || token.data == "textarea" ||
          token.data == "xmp" || token.data == "iframe" || token.data == "noembed") {
        SwitchToRawText(token, token.data == "textarea" ? TokenizerState::RcData
                                                        : (token.data == "script"
                                                               ? TokenizerState::ScriptData
                                                               : TokenizerState::RawText));
        return;
      }
      // A new block-level element implicitly closes an open paragraph. Without
      // this, `<p>a<p>b` nests instead of producing two siblings.
      if (Contains(kClosesParagraph, token.data) && HasInScope("p")) {
        PopUntil("p");
      }
      if (token.data == "li" && HasInScope("li", Scope::ListItem)) {
        GenerateImpliedEndTags("li");
        PopUntil("li");
      }
      if ((token.data == "dd" || token.data == "dt") &&
          (HasInScope("dd") || HasInScope("dt"))) {
        GenerateImpliedEndTags(token.data);
        if (HasInScope("dd")) {
          PopUntil("dd");
        } else {
          PopUntil("dt");
        }
      }
      InsertElement(token);
      return;
    }

    case Token::Kind::EndTag: {
      if (Contains(kUnsupportedTags, token.data)) {
        ++unsupported_;
        return;
      }
      if (token.data == "body" || token.data == "html") {
        if (!HasInScope("body")) {
          ++errors_;
          return;
        }
        mode_ = InsertionMode::AfterBody;
        if (token.data == "html") {
          Process(token);  // reprocess in the new mode
        }
        return;
      }
      if (Contains(kTableStructureTags, token.data)) {
        ++errors_;  // likewise: table structure closing outside a table
        return;
      }
      if (!HasInScope(token.data)) {
        // An end tag for something not open is a parse error and is dropped.
        // Popping anyway would close an unrelated element — `</div>` with no
        // open div must not close the body.
        ++errors_;
        return;
      }
      GenerateImpliedEndTags(token.data);
      PopUntil(token.data);
      return;
    }

    case Token::Kind::EndOfFile:
      return;
  }
}

// §13.2.6.4.9, "reset the insertion mode appropriately". After a table closes,
// the mode follows from what is still open — the parser cannot simply restore
// what it was, because the token that closed the table may have closed more.
void TreeBuilder::ResetInsertionMode() {
  for (std::size_t i = open_elements_.size(); i-- > 0;) {
    const bool last = i == 0;
    // The fragment case, §13.2.6.4.9 step 3: the bottom of a fragment parser's
    // stack is the html element it invented, and the node the algorithm asks
    // about there is the *context* element the nodes are going into. That one
    // substitution is the entire reason `<td>` parsed into a `tr` context
    // becomes a cell and parsed into a `div` context becomes bare text.
    const std::string& name =
        last && !context_tag_name_.empty() ? context_tag_name_ : open_elements_[i]->TagName();
    if (name == "template") {
      // A template restores the mode it was opened in, which is what the stack
      // of template insertion modes is for.
      mode_ = template_modes_.empty() ? InsertionMode::InBody : template_modes_.back();
      return;
    }
    if (!last && (name == "td" || name == "th")) {
      mode_ = InsertionMode::InCell;
      return;
    }
    if (name == "tr") {
      mode_ = InsertionMode::InRow;
      return;
    }
    if (name == "tbody" || name == "thead" || name == "tfoot") {
      mode_ = InsertionMode::InTableBody;
      return;
    }
    if (name == "caption") {
      mode_ = InsertionMode::InCaption;
      return;
    }
    if (name == "colgroup") {
      mode_ = InsertionMode::InColumnGroup;
      return;
    }
    if (name == "table") {
      mode_ = InsertionMode::InTable;
      return;
    }
    if (name == "select") {
      for (std::size_t ancestor = i; ancestor-- > 0;) {
        const std::string& ancestor_name = open_elements_[ancestor]->TagName();
        if (ancestor_name == "table" || ancestor_name == "caption" ||
            ancestor_name == "tbody" || ancestor_name == "tfoot" ||
            ancestor_name == "thead" || ancestor_name == "tr" ||
            ancestor_name == "td" || ancestor_name == "th") {
          mode_ = InsertionMode::InSelectInTable;
          return;
        }
      }
      mode_ = InsertionMode::InSelect;
      return;
    }
    // §13.2.6.4.9 step 12 is "head, and last is false". The last-is-false half
    // matters: a fragment whose context is `head` parses its markup as body
    // content, which is what every other engine does with
    // `document.head.innerHTML = '<title>x</title>'`.
    if (name == "head" && !last) {
      mode_ = InsertionMode::InHead;
      return;
    }
    if (name == "body") {
      mode_ = InsertionMode::InBody;
      return;
    }
    if (name == "html") {
      mode_ = head_ == nullptr ? InsertionMode::BeforeHead : InsertionMode::AfterHead;
      return;
    }
  }
  mode_ = InsertionMode::InBody;
}

bool TreeBuilder::ProcessHeadElement(const Token& token) {
  if (token.kind != Token::Kind::StartTag) {
    return false;
  }
  if (token.data == "title") {
    SwitchToRawText(token, TokenizerState::RcData);
    return true;
  }
  if (token.data == "style" || token.data == "noscript" || token.data == "noframes") {
    SwitchToRawText(token, TokenizerState::RawText);
    return true;
  }
  if (token.data == "script") {
    SwitchToRawText(token, TokenizerState::ScriptData);
    return true;
  }
  if (Contains(kHeadVoidTags, token.data)) {
    InsertElement(token);
    return true;
  }
  return false;
}

// The `shadowrootmode` attribute as the DOM's enumerated value: open, closed, or
// neither. Anything else -- including the empty string and a missing attribute --
// is "none", which is what makes `<template shadowrootmode=invalid>` an ordinary
// template rather than an error.
bool TreeBuilder::ProcessDeclarativeShadowRoot(const Token& token) {
  if (!allow_declarative_shadow_roots_) {
    return false;
  }
  const std::string* mode = nullptr;
  bool delegates_focus = false;
  bool clonable = false;
  bool serializable = false;
  bool manual_slots = false;
  const std::string* adopted_sheets = nullptr;
  for (const Attribute& attribute : token.attributes) {
    if (attribute.name == "shadowrootmode") {
      mode = &attribute.value;
    } else if (attribute.name == "shadowrootdelegatesfocus") {
      delegates_focus = true;
    } else if (attribute.name == "shadowrootclonable") {
      clonable = true;
    } else if (attribute.name == "shadowrootserializable") {
      serializable = true;
    } else if (attribute.name == "shadowrootadoptedstylesheets") {
      adopted_sheets = &attribute.value;
    } else if (attribute.name == "shadowrootslotassignment") {
      // Enumerated with "named" as the invalid-value default, so only the one
      // spelling turns it on and everything else -- including the empty string
      // -- leaves it alone.
      manual_slots = util::AsciiLowerCase(attribute.value) == "manual";
    }
  }
  if (mode == nullptr) {
    return false;
  }
  const std::string folded = util::AsciiLowerCase(*mode);
  if (folded != "open" && folded != "closed") {
    return false;
  }
  // The host is the current node, and it must not be the bottom of the stack.
  // That last condition is what makes a `<template shadowrootmode>` at the root
  // of a fragment an ordinary template: in a fragment parse the bottom is the
  // context element, and a root that attached to it would be a shadow root the
  // caller has no host to reach it through.
  if (open_elements_.size() < 2) {
    return false;
  }
  dom::Element& host = *open_elements_.back();

  dom::ShadowFlags flags = dom::ShadowFlags::Declarative;
  if (folded == "open") {
    flags |= dom::ShadowFlags::Open;
  }
  if (delegates_focus) {
    flags |= dom::ShadowFlags::DelegatesFocus;
  }
  if (clonable) {
    flags |= dom::ShadowFlags::Clonable;
  }
  if (serializable) {
    flags |= dom::ShadowFlags::Serializable;
  }
  if (manual_slots) {
    flags |= dom::ShadowFlags::ManualSlotAssignment;
  }
  const dom::ShadowAttachResult attached = host.AttachShadow(flags);
  if (attached.status != dom::ShadowAttachStatus::Created) {
    // Refused: not a shadow host, or already one. The token becomes an ordinary
    // `<template>` element, which is what the page sees left in the tree.
    ++errors_;
    return false;
  }

  // Kept verbatim rather than resolved: see the accessor's comment in Node.h --
  // resolving a specifier needs an import map, there is no module loader, and an
  // unresolvable one is specified to be skipped silently. This is what lets
  // `getHTML` hand back what the author wrote.
  if (adopted_sheets != nullptr) {
    attached.root->SetAuthoredAdoptedStyleSheetSpecifiers(*adopted_sheets);
  }

  // Created but *not inserted*: the spec adds it to the stack of open elements
  // only. It is owned here for the life of the parse; everything inside lands in
  // its contents, and the flush moves that into the root.
  auto templ = std::make_unique<dom::Element>(token.data);
  for (const Attribute& attribute : token.attributes) {
    templ->SetAttribute(attribute.name, attribute.value);
  }
  open_elements_.push_back(templ.get());
  declarative_shadows_.push_back(DeclarativeShadow{std::move(templ), attached.root});
  return true;
}

void TreeBuilder::FlushDeclarativeShadow(const dom::Element* templ) {
  for (std::size_t i = declarative_shadows_.size(); i-- > 0;) {
    DeclarativeShadow& pending = declarative_shadows_[i];
    if (pending.templ.get() != templ) {
      continue;
    }
    dom::DocumentFragment* content = pending.templ->Content();
    if (content != nullptr && pending.root != nullptr) {
      while (content->FirstChild() != nullptr) {
        std::unique_ptr<dom::Node> moved = content->Detach(content->FirstChild());
        if (moved == nullptr) {
          break;
        }
        pending.root->Append(std::move(moved));
      }
    }
    declarative_shadows_.erase(declarative_shadows_.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

void TreeBuilder::FlushDeclarativeShadows() {
  while (!declarative_shadows_.empty()) {
    FlushDeclarativeShadow(declarative_shadows_.back().templ.get());
  }
}

bool TreeBuilder::HasOpenTemplate() const {
  return std::any_of(open_elements_.begin(), open_elements_.end(),
                     [](const dom::Element* element) { return element->TagName() == "template"; });
}

bool TreeBuilder::ProcessTemplateToken(const Token& token) {
  if (token.data != "template" ||
      (token.kind != Token::Kind::StartTag && token.kind != Token::Kind::EndTag)) {
    return false;
  }
  switch (mode_) {
    // The three modes before the html element exists have to build it first, so
    // a `<template>` there falls through their anything-else clauses and comes
    // back here once the head is open. Text and "in table text" are not modes a
    // tag token reaches at all: one is raw text, the other buffers characters.
    case InsertionMode::Initial:
    case InsertionMode::BeforeHtml:
    case InsertionMode::BeforeHead:
    case InsertionMode::Text:
    case InsertionMode::InTableText:
      return false;
    default:
      break;
  }

  if (token.kind == Token::Kind::StartTag) {
    if (!ProcessDeclarativeShadowRoot(token)) {
      InsertElement(token);
    }
    frameset_ok_ = false;
    template_modes_.push_back(InsertionMode::InTemplate);
    mode_ = InsertionMode::InTemplate;
    return true;
  }
  if (!HasOpenTemplate()) {
    // `</template>` with none open. Popping anyway would close whatever else is
    // on the stack, which is the whole class of bug the scope checks exist for.
    ++errors_;
    return true;
  }
  GenerateImpliedEndTags({}, true);
  if (!open_elements_.empty() && open_elements_.back()->TagName() != "template") {
    ++errors_;
  }
  // Which template this end tag closes has to be found *before* the pop -- the
  // innermost one on the stack -- but flushing it has to happen *after*.
  //
  // The flush destroys the template, because a declarative one is owned by
  // `declarative_shadows_` and by nothing else. Doing it first left a freed
  // pointer on `open_elements_` for `PopUntil` to read `TagName()` off, which
  // is a use-after-free on a path any page can reach with
  // `<div><template shadowrootmode=open></template></div>`. Found by ASan
  // through ShadowDom/DeclarativeTemplateBecomesARootAndLeavesNoTemplate.
  const dom::Element* closing = nullptr;
  for (std::size_t i = open_elements_.size(); i-- > 0;) {
    if (open_elements_[i]->TagName() == "template") {
      closing = open_elements_[i];
      break;
    }
  }
  PopUntil("template");
  if (closing != nullptr) {
    FlushDeclarativeShadow(closing);
  }
  if (!template_modes_.empty()) {
    template_modes_.pop_back();
  }
  ResetInsertionMode();
  return true;
}

// §13.2.6.4.4 "in template". A template's contents are parsed as if they were
// somewhere else entirely: the mode is chosen by the *first* tag inside it, so
// `<template><tr>` builds a row and `<template><div>` builds body content, and
// each choice replaces the entry on the stack of template insertion modes.
void TreeBuilder::ProcessInTemplate(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
    case Token::Kind::Comment:
    case Token::Kind::Doctype:
      ProcessInBody(token);
      return;

    case Token::Kind::EndOfFile:
      if (!HasOpenTemplate()) {
        return;  // nothing left open: the parse is over
      }
      ++errors_;
      PopUntil("template");
      if (!template_modes_.empty()) {
        template_modes_.pop_back();
      }
      ResetInsertionMode();
      Process(token);
      return;

    case Token::Kind::StartTag: {
      if (ProcessHeadElement(token)) {
        return;
      }
      InsertionMode next = InsertionMode::InBody;
      if (token.data == "caption" || token.data == "colgroup" || token.data == "tbody" ||
          token.data == "tfoot" || token.data == "thead") {
        next = InsertionMode::InTable;
      } else if (token.data == "col") {
        next = InsertionMode::InColumnGroup;
      } else if (token.data == "tr") {
        next = InsertionMode::InTableBody;
      } else if (token.data == "td" || token.data == "th") {
        next = InsertionMode::InRow;
      }
      if (!template_modes_.empty()) {
        template_modes_.back() = next;
      }
      mode_ = next;
      Process(token);
      return;
    }

    case Token::Kind::EndTag:
      // Every end tag but `</template>`, which never reaches here. There is no
      // recovery to do: the contents are not in a document, so there is nothing
      // an unbalanced end tag could close its way out into.
      ++errors_;
      return;
  }
}

void TreeBuilder::Process(const Token& token) {
  if (ProcessTemplateToken(token)) {
    return;
  }
  switch (mode_) {
    case InsertionMode::Initial: {
      if (token.kind == Token::Kind::Doctype) {
        // The public and system identifiers travel with the node now. The
        // tokenizer has always produced them -- it needs them for quirks mode
        // -- and the tree dropped them, so `document.doctype.publicId` was the
        // empty string on every page that has one.
        document_->Append(std::make_unique<dom::DocumentType>(
            token.data, token.public_identifier, token.system_identifier));
        // Quirks mode is a rendering decision, and it is decided here once.
        document_->SetQuirksMode(token.force_quirks || token.data != "html");
        mode_ = InsertionMode::BeforeHtml;
        return;
      }
      if (token.kind == Token::Kind::Character && IsWhitespaceOnly(token.data)) {
        return;
      }
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, document_.get());
        return;
      }
      // No doctype at all: quirks mode, and reprocess.
      document_->SetQuirksMode(true);
      mode_ = InsertionMode::BeforeHtml;
      Process(token);
      return;
    }

    case InsertionMode::BeforeHtml: {
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, document_.get());
        return;
      }
      if (token.kind == Token::Kind::Character && IsWhitespaceOnly(token.data)) {
        return;
      }
      if (token.kind == Token::Kind::StartTag && token.data == "html") {
        InsertElement(token);
        mode_ = InsertionMode::BeforeHead;
        return;
      }
      // Every document has an html element whether or not it says so.
      Token implied;
      implied.kind = Token::Kind::StartTag;
      implied.data = "html";
      InsertElement(implied);
      mode_ = InsertionMode::BeforeHead;
      Process(token);
      return;
    }

    case InsertionMode::BeforeHead: {
      if (token.kind == Token::Kind::Character && IsWhitespaceOnly(token.data)) {
        return;
      }
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, nullptr);
        return;
      }
      if (token.kind == Token::Kind::StartTag && token.data == "head") {
        head_ = &InsertElement(token);
        mode_ = InsertionMode::InHead;
        return;
      }
      Token implied;
      implied.kind = Token::Kind::StartTag;
      implied.data = "head";
      head_ = &InsertElement(implied);
      mode_ = InsertionMode::InHead;
      Process(token);
      return;
    }

    case InsertionMode::InHead: {
      if (token.kind == Token::Kind::Character && IsWhitespaceOnly(token.data)) {
        InsertText(token.data);
        return;
      }
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, nullptr);
        return;
      }
      if (ProcessHeadElement(token)) {
        return;
      }
      if (token.kind == Token::Kind::StartTag && token.data == "head") {
        ++errors_;
        return;
      }
      if (token.kind == Token::Kind::EndTag && token.data == "head") {
        PopUntil("head");
        mode_ = InsertionMode::AfterHead;
        return;
      }
      // Anything else ends the head.
      PopUntil("head");
      mode_ = InsertionMode::AfterHead;
      Process(token);
      return;
    }

    case InsertionMode::AfterHead: {
      if (token.kind == Token::Kind::Character && IsWhitespaceOnly(token.data)) {
        InsertText(token.data);
        return;
      }
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, nullptr);
        return;
      }
      if (token.kind == Token::Kind::StartTag && token.data == "body") {
        InsertElement(token);
        frameset_ok_ = false;
        mode_ = InsertionMode::InBody;
        return;
      }
      Token implied;
      implied.kind = Token::Kind::StartTag;
      implied.data = "body";
      InsertElement(implied);
      mode_ = InsertionMode::InBody;
      Process(token);
      return;
    }

    case InsertionMode::InBody:
      ProcessInBody(token);
      return;

    case InsertionMode::InTable:
      ProcessInTable(token);
      return;

    case InsertionMode::InTableText:
      ProcessInTableText(token);
      return;

    case InsertionMode::InCaption:
      ProcessInCaption(token);
      return;

    case InsertionMode::InColumnGroup:
      ProcessInColumnGroup(token);
      return;

    case InsertionMode::InTableBody:
      ProcessInTableBody(token);
      return;

    case InsertionMode::InRow:
      ProcessInRow(token);
      return;

    case InsertionMode::InCell:
      ProcessInCell(token);
      return;

    case InsertionMode::InSelect:
      ProcessInSelect(token);
      return;

    case InsertionMode::InSelectInTable:
      ProcessInSelectInTable(token);
      return;

    case InsertionMode::InTemplate:
      ProcessInTemplate(token);
      return;

    case InsertionMode::Text: {
      if (token.kind == Token::Kind::Character) {
        InsertText(token.data);
        return;
      }
      // Any end tag returns to where we came from. The tokenizer already
      // guaranteed it is the matching one, because RCDATA and RAWTEXT only
      // recognize their own.
      PopCurrent();
      mode_ = original_mode_;
      if (token.kind == Token::Kind::EndOfFile) {
        Process(token);
      }
      return;
    }

    case InsertionMode::AfterBody: {
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, document_->DocumentElement());
        return;
      }
      if (token.kind == Token::Kind::EndTag && token.data == "html") {
        mode_ = InsertionMode::AfterAfterBody;
        return;
      }
      if (token.kind == Token::Kind::EndOfFile) {
        return;
      }
      // Content after </body> goes back into the body, which is what browsers
      // do and what pages with trailing markup depend on.
      ++errors_;
      mode_ = InsertionMode::InBody;
      Process(token);
      return;
    }

    case InsertionMode::AfterAfterBody: {
      if (token.kind == Token::Kind::Comment) {
        InsertComment(token.data, document_.get());
        return;
      }
      if (token.kind == Token::Kind::EndOfFile) {
        return;
      }
      ++errors_;
      mode_ = InsertionMode::InBody;
      Process(token);
      return;
    }
  }
}

std::unique_ptr<dom::Document> TreeBuilder::Build() {
  // Parsing a whole document is the case that needs no opt-in: the markup is the
  // document, so a shadow root in it is the page's own and there is no earlier
  // sanitizing step for one to have slipped past. HTML says the same.
  allow_declarative_shadow_roots_ = true;
  document_ = std::make_unique<dom::Document>();
  AddPerformanceCounter(PerfCounterId::HtmlDocumentsParsed);

  while (const auto token = tokenizer_.Next()) {
    Process(*token);
    if (token->kind == Token::Kind::EndOfFile) {
      break;
    }
  }
  open_elements_.clear();
  // A document that ended inside a `<template shadowrootmode>` still owes its
  // shadow root the content that parsed into it.
  FlushDeclarativeShadows();
  return std::move(document_);
}

// §13.2.6 step 3: which tokenizer state the context element puts the parser in.
// Without it, `titleElement.innerHTML = 'a<b>c'` would build a `<b>` element
// where every other engine produces the text `a<b>c`.
namespace {
TokenizerState FragmentTokenizerState(std::string_view context) {
  if (context == "title" || context == "textarea") {
    return TokenizerState::RcData;
  }
  if (context == "style" || context == "xmp" || context == "iframe" || context == "noembed" ||
      context == "noframes" || context == "noscript") {
    return TokenizerState::RawText;
  }
  if (context == "script") {
    return TokenizerState::ScriptData;
  }
  if (context == "plaintext") {
    return TokenizerState::PlainText;
  }
  return TokenizerState::Data;
}
}  // namespace

std::unique_ptr<dom::DocumentFragment> TreeBuilder::BuildFragment() {
  // A throwaway document, because the algorithm needs a root to hang the parse
  // off and the caller must not be handed one. Nothing here ever reaches the
  // document the fragment is destined for -- which is the point: a fragment
  // that could touch the live tree while parsing would be a page's markup
  // mutating the page mid-parse.
  document_ = std::make_unique<dom::Document>();
  document_->SetQuirksMode(quirks_);
  AddPerformanceCounter(PerfCounterId::HtmlFragmentsParsed);
  AddPerformanceCounter(PerfCounterId::HtmlFragmentBytes, tokenizer_.InputSize());

  tokenizer_.SwitchTo(FragmentTokenizerState(context_tag_name_));
  // So that `</title>` inside a title context is recognized as its end tag
  // rather than as text. §13.2.6 step 4's tokenizer half.
  tokenizer_.SetLastStartTag(context_tag_name_);

  dom::Element& root = InsertImplied("html");
  // From here the root is unpoppable. Every node the parse produces is one of
  // its descendants, and an empty stack would insert into the throwaway
  // Document instead -- where the caller would never see it. Attacker-chosen
  // markup with an attacker-chosen context is exactly the input that finds the
  // end tag which unbalances the stack.
  stack_floor_ = open_elements_.size();
  if (context_tag_name_ == "template") {
    template_modes_.push_back(InsertionMode::InTemplate);
  }
  ResetInsertionMode();

  while (const auto token = tokenizer_.Next()) {
    Process(*token);
    if (token->kind == Token::Kind::EndOfFile) {
      break;
    }
  }
  open_elements_.clear();
  // Before the nodes move out of the root: a fragment that ended inside a
  // `<template shadowrootmode>` still owes its shadow root that content, and the
  // host carrying the root is one of the nodes about to move.
  FlushDeclarativeShadows();

  auto fragment = std::make_unique<dom::DocumentFragment>();
  while (dom::Node* first = root.FirstChild()) {
    std::unique_ptr<dom::Node> moved = root.Detach(first);
    if (moved == nullptr) {
      break;
    }
    fragment->Append(std::move(moved));
  }
  std::size_t nodes = 0;
  fragment->ForEachDescendant([&nodes](const dom::Node&) { ++nodes; });
  AddPerformanceCounter(PerfCounterId::HtmlFragmentNodes, nodes);
  document_.reset();
  return fragment;
}

std::unique_ptr<dom::Document> ParseDocument(std::string_view input) {
  TreeBuilder builder(input);
  return builder.Build();
}

std::unique_ptr<dom::DocumentFragment> ParseFragment(std::string_view input,
                                                     std::string_view context_tag_name, bool quirks,
                                                     bool allow_declarative_shadow_roots) {
  TreeBuilder builder(input, context_tag_name, quirks, allow_declarative_shadow_roots);
  return builder.BuildFragment();
}

}  // namespace microbrowser::html
