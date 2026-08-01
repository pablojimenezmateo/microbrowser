#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Node.h"
#include "html/Token.h"
#include "html/Tokenizer.h"

namespace microbrowser::html {

// Insertion modes, per WHATWG HTML §13.2.6. Named as the spec names them.
//
// Not all of them: the table family, foreign content (SVG and MathML), and
// template contents are not implemented. Each is a substantial algorithm of its
// own, and a *partial* implementation of one is worse than its absence — the
// tree builder would silently produce a tree that differs from every other
// browser, which is the failure this whole spec-literal approach exists to
// avoid. `UnsupportedModeCount()` reports when input needed one, so the gap is
// observable rather than invisible.
enum class InsertionMode : std::uint8_t {
  Initial,
  BeforeHtml,
  BeforeHead,
  InHead,
  AfterHead,
  InBody,
  Text,
  AfterBody,
  AfterAfterBody,
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

  dom::Element& InsertElement(const Token& token);
  void InsertText(std::string_view text);
  void InsertComment(const std::string& data, dom::Node* parent);

  dom::Node& CurrentNode();
  bool HasInScope(std::string_view tag_name) const;
  void PopUntil(std::string_view tag_name);
  void GenerateImpliedEndTags(std::string_view except = {});
  void SwitchToRawText(const Token& token, TokenizerState state);

  Tokenizer tokenizer_;
  std::unique_ptr<dom::Document> document_;
  std::vector<dom::Element*> open_elements_;
  InsertionMode mode_ = InsertionMode::Initial;
  InsertionMode original_mode_ = InsertionMode::Initial;
  dom::Element* head_ = nullptr;
  std::size_t errors_ = 0;
  std::size_t unsupported_ = 0;
  bool frameset_ok_ = true;
};

// Convenience: parse a document in one call.
std::unique_ptr<dom::Document> ParseDocument(std::string_view input);

}  // namespace microbrowser::html
