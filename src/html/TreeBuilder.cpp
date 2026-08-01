#include "html/TreeBuilder.h"

#include <algorithm>
#include <array>

#include "util/PerformanceCounters.h"

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
constexpr std::array<std::string_view, 12> kUnsupportedTags = {
    "table", "tbody", "tfoot", "thead", "tr", "td", "th", "caption", "colgroup", "col",
    "template", "frameset"};

// Elements the spec ends implicitly when a parent closes: `<li>` does not need
// `</li>`, and neither do table cells or definition list items.
constexpr std::array<std::string_view, 8> kImpliedEndTags = {
    "dd", "dt", "li", "optgroup", "option", "p", "rp", "rt"};

bool Contains(const auto& list, std::string_view value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

dom::Node& TreeBuilder::CurrentNode() {
  if (open_elements_.empty()) {
    return *document_;
  }
  return *open_elements_.back();
}

bool TreeBuilder::HasInScope(std::string_view tag_name) const {
  // The real algorithm stops at a scope-marking element (table, td, template,
  // and the foreign-content roots). None of those are implemented, so the walk
  // is the whole stack — correct for every document that does not use them, and
  // those documents are counted as unsupported anyway.
  return std::any_of(open_elements_.begin(), open_elements_.end(),
                     [tag_name](const dom::Element* element) {
                       return element->TagName() == tag_name;
                     });
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
  CurrentNode().Append(std::move(element));
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
  // Appended to the previous text node when there is one, so that a run split
  // across tokens is one node. A DOM with adjacent text nodes is observably
  // different from one without.
  dom::Node* last = CurrentNode().LastChild();
  if (last != nullptr && last->IsText()) {
    static_cast<dom::Text*>(last)->Append(text);
    return;
  }
  CurrentNode().Append(std::make_unique<dom::Text>(std::string(text)));
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
      if (token.data == "li" && HasInScope("li")) {
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
