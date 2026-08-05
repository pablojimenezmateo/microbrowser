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
// Not all of them: foreign content (SVG and MathML) and frameset are not
// implemented. Each is a substantial algorithm of its own, and a *partial*
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
  InTemplate,
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

  // The fragment parser, §13.2.6. `context_tag_name` is the element the markup
  // is going *into*, and it is the whole difference between this and Build():
  // `<td>x</td>` parsed with a `tr` context is a cell, and parsed with a `div`
  // context is the text `x` with the tags dropped. A parser that ignored the
  // context would silently build a different tree from every other browser for
  // every fragment a page assigns to `innerHTML`.
  //
  // `quirks` is the destination document's mode, carried in because it changes
  // whether a `<table>` closes an open `<p>`.
  TreeBuilder(std::string_view input, std::string_view context_tag_name, bool quirks)
      : tokenizer_(input),
        context_tag_name_(context_tag_name.empty() ? "body" : context_tag_name),
        quirks_(quirks) {}

  std::unique_ptr<dom::Document> Build();
  // The parsed nodes, in a fragment with no parent. Never null: HTML has no
  // failure mode, so every input -- including the empty one -- is a fragment.
  std::unique_ptr<dom::DocumentFragment> BuildFragment();

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
  void ProcessInTemplate(const Token& token);
  // The elements the head holds — `base`, `link`, `meta`, `title`, `style`,
  // `script` and the raw-text ones. True when it handled the token. Shared,
  // because "in head" and "in template" both need exactly this set and two
  // copies is how `<title>` ends up starting RCDATA in one of them and not the
  // other.
  bool ProcessHeadElement(const Token& token);
  // §13.2.6.4.4: every insertion mode that mentions `template` says "process
  // using the rules for the in head insertion mode", so there is one
  // implementation of the start and end tags and every mode reaches it. True
  // when it handled the token.
  bool ProcessTemplateToken(const Token& token);
  bool HasOpenTemplate() const;

  dom::Element& InsertElement(const Token& token);
  void InsertText(std::string_view text);
  void InsertComment(const std::string& data, dom::Node* parent);
  // Inserts an element the source did not write: an implied tbody around a
  // stray `<tr>`, an implied html around everything.
  dom::Element& InsertImplied(std::string_view tag_name);

  dom::Node& CurrentNode();
  bool HasInScope(std::string_view tag_name, Scope scope = Scope::Default) const;
  // Pops the current node, unless that would take the stack below the floor —
  // see stack_floor_. Every pop in this class goes through it.
  void PopCurrent();
  void PopUntil(std::string_view tag_name);
  // Pops until the current node is one of `context` — or the html element,
  // which stops every one of these walks. §13.2.6.4.9 and its neighbours.
  void ClearStackToContext(std::initializer_list<std::string_view> context);
  // `thoroughly` is the spec's wider list, which also ends the table elements.
  // Only `</template>` uses it, and only because a template may be closed with
  // a row still open inside it.
  void GenerateImpliedEndTags(std::string_view except = {}, bool thoroughly = false);
  void SwitchToRawText(const Token& token, TokenizerState state);
  // §13.2.6.4.9 "reset the insertion mode appropriately": after a table closes,
  // the mode is a function of what is still open, not of what it was before.
  void ResetInsertionMode();
  void CloseCell();

  Tokenizer tokenizer_;
  std::unique_ptr<dom::Document> document_;
  std::vector<dom::Element*> open_elements_;
  // The stack of template insertion modes, §13.2.4.4. One entry per open
  // `<template>`: closing one restores the mode the template was opened in,
  // which a single saved mode could not do for a template inside a template.
  std::vector<InsertionMode> template_modes_;
  // The element a fragment is being parsed into, and empty for a whole
  // document. It is also *how* the builder knows which of the two it is doing:
  // a separate `fragment_` flag would be a second answer to one question, and
  // the two disagreeing is a tree built with the wrong rules.
  std::string context_tag_name_;
  InsertionMode mode_ = InsertionMode::Initial;
  InsertionMode original_mode_ = InsertionMode::Initial;
  dom::Element* head_ = nullptr;
  // Character tokens seen in a table, held until something else arrives: the
  // spec cannot decide where they go until it knows whether the run is only
  // whitespace.
  std::string pending_table_text_;
  std::size_t errors_ = 0;
  std::size_t unsupported_ = 0;
  // How many elements at the bottom of the open-element stack no error recovery
  // may pop. Zero for a document; one for a fragment, whose root html element
  // is where every parsed node lands — an empty stack there would insert into
  // the throwaway Document instead, and those nodes would never reach the
  // caller. Attacker-chosen markup with an attacker-chosen context is exactly
  // the input that finds the end tag which unbalances the stack.
  std::size_t stack_floor_ = 0;
  bool frameset_ok_ = true;
  // Set while running the "anything else" clauses of the table modes, which
  // insert *before* the table rather than into it.
  bool foster_parenting_ = false;
  // The destination document's mode, for a fragment. A whole document decides
  // this from its own doctype instead.
  bool quirks_ = false;
};

// Convenience: parse a document in one call.
std::unique_ptr<dom::Document> ParseDocument(std::string_view input);

// The HTML fragment parsing algorithm in one call, §13.2.6. `context_tag_name`
// is the element the nodes are going into; see the constructor above for why it
// is not optional. This is the entry point `innerHTML`, `insertAdjacentHTML`
// and `<template>` reach, which makes it the most hostile input path in the
// browser: bytes chosen by a page, parsed with a context chosen by a page.
std::unique_ptr<dom::DocumentFragment> ParseFragment(std::string_view input,
                                                     std::string_view context_tag_name,
                                                     bool quirks = false);

}  // namespace microbrowser::html
