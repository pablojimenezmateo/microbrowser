#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "html/Token.h"

namespace microbrowser::html {

// The HTML tokenizer, per WHATWG HTML §13.2.5.
//
// Spec-literal, and the repo's rule for spec parsers applies in full: **the
// states below are named exactly as the specification names them**, in the same
// order, so this file can be read against the spec section by section.
// Divergence from the spec text is a bug rather than a style choice.
//
// That is not reverence. HTML's error handling is not a quality-of-implementation
// matter — it is normative, every browser implements the same recovery, and
// pages depend on it. A tokenizer that is "reasonable" instead of correct
// produces a different DOM from every other browser for input that is, in
// practice, most of the web.
enum class TokenizerState : std::uint8_t {
  Data,
  RcData,
  RawText,
  ScriptData,
  PlainText,
  TagOpen,
  EndTagOpen,
  TagName,
  RcDataLessThanSign,
  RcDataEndTagOpen,
  RcDataEndTagName,
  RawTextLessThanSign,
  RawTextEndTagOpen,
  RawTextEndTagName,
  ScriptDataLessThanSign,
  ScriptDataEndTagOpen,
  ScriptDataEndTagName,
  BeforeAttributeName,
  AttributeName,
  AfterAttributeName,
  BeforeAttributeValue,
  AttributeValueDoubleQuoted,
  AttributeValueSingleQuoted,
  AttributeValueUnquoted,
  AfterAttributeValueQuoted,
  SelfClosingStartTag,
  BogusComment,
  MarkupDeclarationOpen,
  CommentStart,
  CommentStartDash,
  Comment,
  CommentLessThanSign,
  CommentEndDash,
  CommentEnd,
  CommentEndBang,
  Doctype,
  BeforeDoctypeName,
  DoctypeName,
  AfterDoctypeName,
  AfterDoctypePublicKeyword,
  BeforeDoctypePublicIdentifier,
  DoctypePublicIdentifierDoubleQuoted,
  DoctypePublicIdentifierSingleQuoted,
  AfterDoctypePublicIdentifier,
  BetweenDoctypePublicAndSystemIdentifiers,
  AfterDoctypeSystemKeyword,
  BeforeDoctypeSystemIdentifier,
  DoctypeSystemIdentifierDoubleQuoted,
  DoctypeSystemIdentifierSingleQuoted,
  AfterDoctypeSystemIdentifier,
  BogusDoctype,
  CharacterReference,
  NamedCharacterReference,
  AmbiguousAmpersand,
  NumericCharacterReference,
  HexadecimalCharacterReferenceStart,
  DecimalCharacterReferenceStart,
  HexadecimalCharacterReference,
  DecimalCharacterReference,
  NumericCharacterReferenceEnd,
};

class Tokenizer {
 public:
  explicit Tokenizer(std::string_view input) : input_(input) {}

  // Nullopt once the end-of-file token has been returned. Every other outcome,
  // including every parse error, produces a token — the spec has no failure
  // mode, and neither does this.
  std::optional<Token> Next();

  // The tree builder switches the tokenizer into RCDATA, RAWTEXT, script or
  // plaintext when it sees the elements that need them. It has to be the tree
  // builder that does it, because whether `<title>` means "start RCDATA"
  // depends on where in the tree it appeared.
  void SwitchTo(TokenizerState state) { state_ = state; }
  TokenizerState State() const { return state_; }

  // Set by the tree builder so an end tag inside RCDATA or RAWTEXT is only
  // recognized when it closes the element that opened the mode. `</b>` inside
  // `<title>` is text, not a tag.
  void SetLastStartTag(std::string name) { last_start_tag_ = std::move(name); }

  std::size_t ErrorCount() const { return errors_; }
  // How many bytes it was given. The fragment counters report it, because "a
  // page spent its time in innerHTML" and "a page spent its time parsing a
  // megabyte through innerHTML" call for different fixes.
  std::size_t InputSize() const { return input_.size(); }

 private:
  int Peek(std::size_t ahead = 0) const;
  int Consume();
  void Reconsume() { --position_; }
  bool ConsumeIfMatch(std::string_view text, bool case_insensitive);

  void Error() { ++errors_; }
  void EmitCharacter(char c) { pending_characters_.push_back(c); }
  bool AppropriateEndTag() const;

  std::string_view input_;
  std::size_t position_ = 0;
  TokenizerState state_ = TokenizerState::Data;
  TokenizerState return_state_ = TokenizerState::Data;

  Token current_;
  Attribute current_attribute_;
  bool has_current_attribute_ = false;
  std::string temporary_buffer_;
  std::string character_reference_code_text_;
  std::uint32_t character_reference_code_ = 0;
  std::string last_start_tag_;
  std::string pending_characters_;
  std::vector<Token> queued_;
  bool emitted_eof_ = false;
  std::size_t errors_ = 0;

  void FlushCharacters();
  void EmitCurrent();
  void FinishAttribute();
};

// Resolves a named character reference such as `&amp;` or `&nbsp;`. Returns the
// UTF-8 expansion and how many input characters it consumed, or zero length
// when the name is not one we know.
struct NamedReference {
  std::string replacement;
  std::size_t consumed = 0;
};
NamedReference LookUpNamedCharacterReference(std::string_view input);

// Appends `code_point` as UTF-8.
void AppendUtf8(std::uint32_t code_point, std::string& out);

}  // namespace microbrowser::html
