#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Node.h"
#include "html/Token.h"
#include "html/Tokenizer.h"

namespace microbrowser::html {

// Insertion modes, per WHATWG HTML §13.2.6. Named as the spec names them, in
// the spec's order.
//
// Not all of them: foreign content (SVG and MathML) and template contents are
// not implemented. Each is a substantial algorithm of its own, and a *partial*
// implementation of one is worse than its absence — the tree builder would
// silently produce a tree that differs from every other browser, which is the
// failure this whole spec-literal approach exists to avoid.
enum class InsertionMode : std::uint8_t {
  Initial,
  BeforeHtml,
  BeforeHead,
  InHead,
  AfterHead,
  InBody,
  Text,
  InTable,
  InTableText,
  InCaption,
  InColumnGroup,
  InTableBody,
  InRow,
  InCell,
  InSelect,
  InSelectInTable,
  AfterBody,
  AfterAfterBody,
};

// The stack-of-open-elements scopes, §13.2.4.2. They differ only in which
// elements stop the walk, and the difference is load-bearing: `<p>` closes on a
// new block only within button scope, and a `</tr>` must not reach past its own
// table.
enum class Scope : std::uint8_t {
  Default,
  Button,
  ListItem,
  Table,
  Select,
};

// Builds a DOM from HTML source.
//
// The tree builder drives the tokenizer rather than the other way round,
// because the tokenizer's mode depends on the tree: whether `<title>` starts
// RCDATA is a question about where in the document it appeared.
class TreeBuilder {
 public:
  explicit TreeBuilder(std::string_view input) : tokenizer_(input) {}

  std::unique_ptr<dom::Document> Build();

  std::size_t ParseErrors() const { return errors_ + tokenizer_.ErrorCount(); }
  // How many tokens needed an insertion mode this builder does not implement.
  // Non-zero means the resulting tree is not what a complete parser would
  // produce, and a caller that cares can say so.
  std::size_t UnsupportedModeCount() const { return unsupported_; }

 private:
  void Process(const Token& token);
  void ProcessInBody(const Token& token);
  void ProcessInTable(const Token& token);
  void ProcessInTableText(const Token& token);
  void ProcessInCaption(const Token& token);
  void ProcessInColumnGroup(const Token& token);
  void ProcessInTableBody(const Token& token);
  void ProcessInRow(const Token& token);
  void ProcessInCell(const Token& token);
  void ProcessInSelect(const Token& token);
  void ProcessInSelectInTable(const Token& token);

  dom::Element& InsertElement(const Token& token);
  void InsertText(std::string_view text);
  void InsertComment(const std::string& data, dom::Node* parent);
  // Inserts an element the source did not write: an implied tbody around a
  // stray `<tr>`, an implied html around everything.
  dom::Element& InsertImplied(std::string_view tag_name);

  dom::Node& CurrentNode();
  bool HasInScope(std::string_view tag_name, Scope scope = Scope::Default) const;
  void PopUntil(std::string_view tag_name);
  // Pops until the current node is one of `context` — or the html element,
  // which stops every one of these walks. §13.2.6.4.9 and its neighbours.
  void ClearStackToContext(std::initializer_list<std::string_view> context);
  void GenerateImpliedEndTags(std::string_view except = {});
  void SwitchToRawText(const Token& token, TokenizerState state);
  // §13.2.6.4.9 "reset the insertion mode appropriately": after a table closes,
  // the mode is a function of what is still open, not of what it was before.
  void ResetInsertionMode();
  void CloseCell();

  Tokenizer tokenizer_;
  std::unique_ptr<dom::Document> document_;
  std::vector<dom::Element*> open_elements_;
  InsertionMode mode_ = InsertionMode::Initial;
  InsertionMode original_mode_ = InsertionMode::Initial;
  dom::Element* head_ = nullptr;
  // Character tokens seen in a table, held until something else arrives: the
  // spec cannot decide where they go until it knows whether the run is only
  // whitespace.
  std::string pending_table_text_;
  std::size_t errors_ = 0;
  std::size_t unsupported_ = 0;
  bool frameset_ok_ = true;
  // Set while running the "anything else" clauses of the table modes, which
  // insert *before* the table rather than into it.
  bool foster_parenting_ = false;
};

// Convenience: parse a document in one call.
std::unique_ptr<dom::Document> ParseDocument(std::string_view input);

}  // namespace microbrowser::html
