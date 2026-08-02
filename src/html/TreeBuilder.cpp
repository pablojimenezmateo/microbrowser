#include "html/TreeBuilder.h"

#include <algorithm>
#include <array>

#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

namespace microbrowser::html {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

bool IsWhitespaceOnly(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
  });
}

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
constexpr std::array<std::string_view, 2> kUnsupportedTags = {"template", "frameset"};

// Elements the spec ends implicitly when a parent closes: `<li>` does not need
// `</li>`, and neither do table cells or definition list items.
constexpr std::array<std::string_view, 8> kImpliedEndTags = {
    "dd", "dt", "li", "optgroup", "option", "p", "rp", "rt"};

// §13.2.4.2. The elements that stop a scope walk. The foreign-content roots
// belong here too and are absent because foreign content is.
constexpr std::array<std::string_view, 8> kScopeStoppers = {
    "applet", "caption", "html", "marquee", "object", "table", "td", "th"};

// Everything a table start tag may open, which is also the set that an
// unexpected one of them closes its way out to.
constexpr std::array<std::string_view, 9> kTableStructureTags = {
    "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"};

constexpr std::array<std::string_view, 8> kSelectTableTags = {
    "caption", "table", "tbody", "tfoot", "thead", "tr", "td", "th"};

bool Contains(const auto& list, std::string_view value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

// True for the elements whose children must be table structure, and into which
// text and stray elements are therefore never inserted directly.
bool IsFosterParent(std::string_view tag_name) {
  return tag_name == "table" || tag_name == "tbody" || tag_name == "tfoot" ||
         tag_name == "thead" || tag_name == "tr";
}

// §13.2.6.1, "the appropriate place for inserting a node": normally the current
// node, but inside a table with foster parenting on, immediately before the
// table. `before` is null for an append.
struct InsertionPoint {
  dom::Node* parent = nullptr;
  const dom::Node* before = nullptr;
};

// §13.2.6.1, "the appropriate place for inserting a node". A free function
// rather than a member because its return type is local to this file, and the
// header should not have to name it.
InsertionPoint AppropriatePlace(const std::vector<dom::Element*>& stack, bool foster_parenting,
                                dom::Document& document) {
  dom::Node* current =
      stack.empty() ? static_cast<dom::Node*>(&document) : static_cast<dom::Node*>(stack.back());
  if (!foster_parenting || stack.empty() || !IsFosterParent(stack.back()->TagName())) {
    return {current, nullptr};
  }
  for (std::size_t i = stack.size(); i-- > 0;) {
    dom::Element* table = stack[i];
    if (table->TagName() != "table") {
      continue;
    }
    if (table->Parent() != nullptr) {
      return {table->Parent(), table};
    }
    // A table that is not in the tree cannot be inserted before. The spec's
    // fallback is the element below it on the stack.
    return {i > 0 ? static_cast<dom::Node*>(stack[i - 1]) : current, nullptr};
  }
  return {static_cast<dom::Node*>(stack.front()), nullptr};
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

void TreeBuilder::PopUntil(std::string_view tag_name) {
  while (!open_elements_.empty()) {
    const bool matched = open_elements_.back()->TagName() == tag_name;
    open_elements_.pop_back();
    if (matched) {
      return;
    }
  }
}

void TreeBuilder::ClearStackToContext(std::initializer_list<std::string_view> context) {
  while (!open_elements_.empty()) {
    const std::string& name = open_elements_.back()->TagName();
    if (name == "html" || Contains(context, name)) {
      return;
    }
    open_elements_.pop_back();
  }
}

void TreeBuilder::GenerateImpliedEndTags(std::string_view except) {
  while (!open_elements_.empty()) {
    const std::string& name = open_elements_.back()->TagName();
    if (name == except || !Contains(kImpliedEndTags, name)) {
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
  if (!dom::IsVoidElement(token.data) && !token.self_closing) {
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

// §13.2.6.4.9 "in table".
//
// The table modes exist because a table's children are constrained in a way no
// other element's are: text and stray elements cannot live between a `<table>`
// and its `<tr>`, so the parser moves them out (foster parenting) rather than
// building a tree no layout engine could interpret. Every clause below is the
// spec's; the ones that are absent are template and select.
void TreeBuilder::ProcessInTable(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
      if (!open_elements_.empty() && IsFosterParent(open_elements_.back()->TagName())) {
        pending_table_text_.clear();
        original_mode_ = mode_;
        mode_ = InsertionMode::InTableText;
        Process(token);
        return;
      }
      break;  // text somewhere a table cannot hold it: the anything-else clause

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag: {
      if (token.data == "caption") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InCaption;
        return;
      }
      if (token.data == "colgroup") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InColumnGroup;
        return;
      }
      if (token.data == "col") {
        ClearStackToContext({"table"});
        InsertImplied("colgroup");
        mode_ = InsertionMode::InColumnGroup;
        Process(token);
        return;
      }
      if (token.data == "tbody" || token.data == "tfoot" || token.data == "thead") {
        ClearStackToContext({"table"});
        InsertElement(token);
        mode_ = InsertionMode::InTableBody;
        return;
      }
      if (token.data == "td" || token.data == "th" || token.data == "tr") {
        // A row that skipped its section gets one. Pages write this constantly.
        ClearStackToContext({"table"});
        InsertImplied("tbody");
        mode_ = InsertionMode::InTableBody;
        Process(token);
        return;
      }
      if (token.data == "table") {
        // A table inside a table's own structure closes the outer one, which is
        // not what the markup says and is what every browser does.
        ++errors_;
        if (!HasInScope("table", Scope::Table)) {
          return;
        }
        PopUntil("table");
        ResetInsertionMode();
        Process(token);
        return;
      }
      if (token.data == "style") {
        SwitchToRawText(token, TokenizerState::RawText);
        return;
      }
      if (token.data == "script") {
        SwitchToRawText(token, TokenizerState::ScriptData);
        return;
      }
      if (token.data == "input" && token.AttributeValue("type") != nullptr &&
          util::EqualsAsciiCaseInsensitive(*token.AttributeValue("type"), "hidden")) {
        // The one element allowed to sit in a table without being fostered: it
        // renders nothing, so it cannot disturb the table's boxes.
        ++errors_;
        InsertElement(token);
        return;
      }
      break;
    }

    case Token::Kind::EndTag: {
      if (token.data == "table") {
        if (!HasInScope("table", Scope::Table)) {
          ++errors_;
          return;
        }
        PopUntil("table");
        ResetInsertionMode();
        return;
      }
      if (token.data == "body" || token.data == "html" ||
          Contains(kTableStructureTags, token.data)) {
        ++errors_;
        return;
      }
      break;
    }

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  // Anything else. Foster parenting is the whole point: the token is processed
  // by the ordinary body rules, but whatever it inserts lands before the table
  // instead of inside it.
  ++errors_;
  foster_parenting_ = true;
  ProcessInBody(token);
  foster_parenting_ = false;
}

// §13.2.6.4.10 "in table text". Character tokens are held because where they go
// depends on whether the whole run is whitespace, and that is not known until
// the run ends.
void TreeBuilder::ProcessInTableText(const Token& token) {
  if (token.kind == Token::Kind::Character) {
    pending_table_text_ += token.data;
    return;
  }
  const std::string pending = std::move(pending_table_text_);
  pending_table_text_.clear();
  mode_ = original_mode_;
  if (!pending.empty()) {
    if (IsWhitespaceOnly(pending)) {
      InsertText(pending);
    } else {
      // Non-whitespace in a table is the anything-else clause of "in table",
      // one character token's worth at a time — here, the whole run at once,
      // which produces the same tree.
      ++errors_;
      foster_parenting_ = true;
      InsertText(pending);
      foster_parenting_ = false;
      frameset_ok_ = false;
    }
  }
  Process(token);
}

// §13.2.6.4.11 "in caption".
void TreeBuilder::ProcessInCaption(const Token& token) {
  const bool closes_caption =
      (token.kind == Token::Kind::StartTag && Contains(kTableStructureTags, token.data)) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (closes_caption || (token.kind == Token::Kind::EndTag && token.data == "caption")) {
    if (!HasInScope("caption", Scope::Table)) {
      ++errors_;
      return;
    }
    GenerateImpliedEndTags();
    if (CurrentNode().IsElement() &&
        static_cast<dom::Element&>(CurrentNode()).TagName() != "caption") {
      ++errors_;
    }
    PopUntil("caption");
    mode_ = InsertionMode::InTable;
    if (closes_caption) {
      Process(token);  // the token that forced the close still has to be handled
    }
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "col" || token.data == "colgroup" ||
       token.data == "html" || token.data == "tbody" || token.data == "td" ||
       token.data == "tfoot" || token.data == "th" || token.data == "thead" ||
       token.data == "tr")) {
    ++errors_;
    return;
  }
  ProcessInBody(token);
}

// §13.2.6.4.12 "in column group".
void TreeBuilder::ProcessInColumnGroup(const Token& token) {
  switch (token.kind) {
    case Token::Kind::Character:
      if (IsWhitespaceOnly(token.data)) {
        InsertText(token.data);
        return;
      }
      break;

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag:
      if (token.data == "html") {
        ProcessInBody(token);
        return;
      }
      if (token.data == "col") {
        InsertElement(token);  // void: never becomes the current node
        return;
      }
      break;

    case Token::Kind::EndTag:
      if (token.data == "colgroup") {
        if (open_elements_.empty() || open_elements_.back()->TagName() != "colgroup") {
          ++errors_;
          return;
        }
        open_elements_.pop_back();
        mode_ = InsertionMode::InTable;
        return;
      }
      if (token.data == "col") {
        ++errors_;
        return;
      }
      break;

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  if (open_elements_.empty() || open_elements_.back()->TagName() != "colgroup") {
    ++errors_;
    return;
  }
  open_elements_.pop_back();
  mode_ = InsertionMode::InTable;
  Process(token);
}

// §13.2.6.4.13 "in table body".
void TreeBuilder::ProcessInTableBody(const Token& token) {
  if (token.kind == Token::Kind::StartTag) {
    if (token.data == "tr") {
      ClearStackToContext({"tbody", "tfoot", "thead"});
      InsertElement(token);
      mode_ = InsertionMode::InRow;
      return;
    }
    if (token.data == "th" || token.data == "td") {
      ++errors_;  // a cell that skipped its row gets one
      ClearStackToContext({"tbody", "tfoot", "thead"});
      InsertImplied("tr");
      mode_ = InsertionMode::InRow;
      Process(token);
      return;
    }
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tbody", "tfoot", "thead"});
    open_elements_.pop_back();
    mode_ = InsertionMode::InTable;
    return;
  }

  const bool leaves_section =
      (token.kind == Token::Kind::StartTag &&
       (token.data == "caption" || token.data == "col" || token.data == "colgroup" ||
        token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (leaves_section) {
    if (!HasInScope("tbody", Scope::Table) && !HasInScope("thead", Scope::Table) &&
        !HasInScope("tfoot", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tbody", "tfoot", "thead"});
    open_elements_.pop_back();
    mode_ = InsertionMode::InTable;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html" || token.data == "td" ||
       token.data == "th" || token.data == "tr")) {
    ++errors_;
    return;
  }
  ProcessInTable(token);
}

// §13.2.6.4.14 "in row".
void TreeBuilder::ProcessInRow(const Token& token) {
  if (token.kind == Token::Kind::StartTag && (token.data == "th" || token.data == "td")) {
    ClearStackToContext({"tr"});
    InsertElement(token);
    mode_ = InsertionMode::InCell;
    return;
  }
  if (token.kind == Token::Kind::EndTag && token.data == "tr") {
    if (!HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    open_elements_.pop_back();
    mode_ = InsertionMode::InTableBody;
    return;
  }

  const bool leaves_row =
      (token.kind == Token::Kind::StartTag &&
       (token.data == "caption" || token.data == "col" || token.data == "colgroup" ||
        token.data == "tbody" || token.data == "tfoot" || token.data == "thead" ||
        token.data == "tr")) ||
      (token.kind == Token::Kind::EndTag && token.data == "table");
  if (leaves_row) {
    if (!HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    open_elements_.pop_back();
    mode_ = InsertionMode::InTableBody;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "tbody" || token.data == "tfoot" || token.data == "thead")) {
    if (!HasInScope(token.data, Scope::Table) || !HasInScope("tr", Scope::Table)) {
      ++errors_;
      return;
    }
    ClearStackToContext({"tr"});
    open_elements_.pop_back();
    mode_ = InsertionMode::InTableBody;
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html" || token.data == "td" ||
       token.data == "th")) {
    ++errors_;
    return;
  }
  ProcessInTable(token);
}

void TreeBuilder::CloseCell() {
  const std::string_view name = HasInScope("td", Scope::Table) ? "td" : "th";
  GenerateImpliedEndTags();
  if (CurrentNode().IsElement() && static_cast<dom::Element&>(CurrentNode()).TagName() != name) {
    ++errors_;
  }
  PopUntil(name);
  mode_ = InsertionMode::InRow;
}

// §13.2.6.4.15 "in cell". A cell is the one place inside a table where ordinary
// content is ordinary again, so most tokens go straight to the body rules.
void TreeBuilder::ProcessInCell(const Token& token) {
  if (token.kind == Token::Kind::EndTag && (token.data == "td" || token.data == "th")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    GenerateImpliedEndTags();
    if (CurrentNode().IsElement() &&
        static_cast<dom::Element&>(CurrentNode()).TagName() != token.data) {
      ++errors_;
    }
    PopUntil(token.data);
    mode_ = InsertionMode::InRow;
    return;
  }
  if (token.kind == Token::Kind::StartTag && Contains(kTableStructureTags, token.data)) {
    // A new cell or row ends this one: `<td>a<td>b` is two cells, not nesting.
    if (!HasInScope("td", Scope::Table) && !HasInScope("th", Scope::Table)) {
      ++errors_;
      return;
    }
    CloseCell();
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "table" || token.data == "tbody" || token.data == "tfoot" ||
       token.data == "thead" || token.data == "tr")) {
    if (!HasInScope(token.data, Scope::Table)) {
      ++errors_;
      return;
    }
    CloseCell();
    Process(token);
    return;
  }
  if (token.kind == Token::Kind::EndTag &&
      (token.data == "body" || token.data == "caption" || token.data == "col" ||
       token.data == "colgroup" || token.data == "html")) {
    ++errors_;
    return;
  }
  ProcessInBody(token);
}

// §13.2.6.4.16 "in select".
void TreeBuilder::ProcessInSelect(const Token& token) {
  const auto current_is = [this](std::string_view tag_name) {
    return CurrentNode().IsElement() &&
           static_cast<dom::Element&>(CurrentNode()).TagName() == tag_name;
  };
  const auto pop_current_if = [&](std::string_view tag_name) {
    if (current_is(tag_name)) {
      open_elements_.pop_back();
      return true;
    }
    return false;
  };

  switch (token.kind) {
    case Token::Kind::Character:
      InsertText(token.data);
      return;

    case Token::Kind::Comment:
      InsertComment(token.data, nullptr);
      return;

    case Token::Kind::Doctype:
      ++errors_;
      return;

    case Token::Kind::StartTag:
      if (token.data == "html") {
        ProcessInBody(token);
        return;
      }
      if (token.data == "option") {
        pop_current_if("option");
        InsertElement(token);
        return;
      }
      if (token.data == "optgroup") {
        pop_current_if("option");
        pop_current_if("optgroup");
        InsertElement(token);
        return;
      }
      if (token.data == "select") {
        ++errors_;
        if (!HasInScope("select", Scope::Select)) {
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        return;
      }
      if (token.data == "input" || token.data == "textarea") {
        ++errors_;
        if (!HasInScope("select", Scope::Select)) {
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        Process(token);
        return;
      }
      if (token.data == "script" || token.data == "style") {
        SwitchToRawText(token, token.data == "script" ? TokenizerState::ScriptData
                                                      : TokenizerState::RawText);
        return;
      }
      break;

    case Token::Kind::EndTag:
      if (token.data == "option") {
        if (!pop_current_if("option")) {
          ++errors_;
        }
        return;
      }
      if (token.data == "optgroup") {
        pop_current_if("option");
        if (!pop_current_if("optgroup")) {
          ++errors_;
        }
        return;
      }
      if (token.data == "select") {
        if (!HasInScope("select", Scope::Select)) {
          ++errors_;
          return;
        }
        PopUntil("select");
        ResetInsertionMode();
        return;
      }
      break;

    case Token::Kind::EndOfFile:
      ProcessInBody(token);
      return;
  }

  ++errors_;
}

// §13.2.6.4.17 "in select in table".
void TreeBuilder::ProcessInSelectInTable(const Token& token) {
  const bool table_start = token.kind == Token::Kind::StartTag &&
                           Contains(kSelectTableTags, token.data);
  const bool table_end = token.kind == Token::Kind::EndTag && Contains(kSelectTableTags, token.data);
  if (table_start || table_end) {
    ++errors_;
    if (table_start || HasInScope(token.data, Scope::Table)) {
      PopUntil("select");
      ResetInsertionMode();
      Process(token);
    }
    return;
  }
  ProcessInSelect(token);
}

// §13.2.6.4.9, "reset the insertion mode appropriately". After a table closes,
// the mode follows from what is still open — the parser cannot simply restore
// what it was, because the token that closed the table may have closed more.
void TreeBuilder::ResetInsertionMode() {
  for (std::size_t i = open_elements_.size(); i-- > 0;) {
    const std::string& name = open_elements_[i]->TagName();
    const bool last = i == 0;
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
    if (name == "head" || name == "body") {
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

void TreeBuilder::Process(const Token& token) {
  switch (mode_) {
    case InsertionMode::Initial: {
      if (token.kind == Token::Kind::Doctype) {
        document_->Append(std::make_unique<dom::DocumentType>(token.data));
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
      if (token.kind == Token::Kind::StartTag) {
        if (token.data == "title") {
          SwitchToRawText(token, TokenizerState::RcData);
          return;
        }
        if (token.data == "style" || token.data == "noscript") {
          SwitchToRawText(token, TokenizerState::RawText);
          return;
        }
        if (token.data == "script") {
          SwitchToRawText(token, TokenizerState::ScriptData);
          return;
        }
        if (token.data == "base" || token.data == "link" || token.data == "meta") {
          InsertElement(token);
          return;
        }
        if (token.data == "head") {
          ++errors_;
          return;
        }
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

    case InsertionMode::Text: {
      if (token.kind == Token::Kind::Character) {
        InsertText(token.data);
        return;
      }
      // Any end tag returns to where we came from. The tokenizer already
      // guaranteed it is the matching one, because RCDATA and RAWTEXT only
      // recognize their own.
      if (!open_elements_.empty()) {
        open_elements_.pop_back();
      }
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
  document_ = std::make_unique<dom::Document>();
  AddPerformanceCounter(PerfCounterId::HtmlDocumentsParsed);

  while (const auto token = tokenizer_.Next()) {
    Process(*token);
    if (token->kind == Token::Kind::EndOfFile) {
      break;
    }
  }
  open_elements_.clear();
  return std::move(document_);
}

std::unique_ptr<dom::Document> ParseDocument(std::string_view input) {
  TreeBuilder builder(input);
  return builder.Build();
}

}  // namespace microbrowser::html
