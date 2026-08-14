#include "html/Tokenizer.h"

#include <algorithm>
#include <array>

#include "util/PerformanceCounters.h"

namespace microbrowser::html {

namespace {

// The HTML Standard's 2,231 named character references, generated from the same `entities.json`
// the suite's own entity tests are written against. `tools/html/generate_entities.py` writes it;
// an `.inc` rather than a header because it is a table included once, inside this anonymous
// namespace, and because that is the shape `src/text`'s generated Unicode tables already have.
#include "html/NamedCharacterReferences.inc"

using util::AddPerformanceCounter;
using util::PerfCounterId;

constexpr int kEof = -1;

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool IsAsciiAlpha(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiAlphanumeric(int c) {
  return IsAsciiAlpha(c) || (c >= '0' && c <= '9');
}

bool IsWhitespace(int c) {
  return c == '\t' || c == '\n' || c == '\f' || c == ' ';
}

// Windows-1252 replacements for the C1 range. Not a compatibility nicety: the
// spec *requires* it, because a decade of documents declared UTF-8 and emitted
// these, and every browser maps them the same way.
constexpr std::array<std::uint32_t, 32> kC1Replacements = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

}  // namespace

void AppendUtf8(std::uint32_t code_point, std::string& out) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

NamedReference LookUpNamedCharacterReference(std::string_view input) {
  NamedReference best;
  if (input.empty()) {
    return best;
  }
  // One letter's run rather than all 2,231. The table is sorted, so every name beginning with the
  // same byte is contiguous, and the generator writes the bounds out. The largest run is 168.
  //
  // **Longest match, not first match**, and the difference is not academic: `&not` and `&notin;`
  // are both references, so `&notin;` read first-match becomes `¬in;`. The specification's own
  // wording is "the longest sequence of characters that is one of the identifiers".
  for (const NamedCharacterReferenceBucket& bucket : kNamedCharacterReferenceBuckets) {
    if (bucket.first != input.front()) {
      continue;
    }
    for (std::size_t i = bucket.begin; i < bucket.end; ++i) {
      const NamedCharacterReference& entity = kNamedCharacterReferences[i];
      if (entity.name.size() <= input.size() && entity.name.size() > best.consumed &&
          input.compare(0, entity.name.size(), entity.name) == 0) {
        best.replacement = std::string(entity.replacement);
        best.consumed = entity.name.size();
      }
    }
    break;
  }
  return best;
}

const std::string* Token::AttributeValue(std::string_view name) const {
  for (const Attribute& attribute : attributes) {
    if (attribute.name == name) {
      return &attribute.value;
    }
  }
  return nullptr;
}

int Tokenizer::Peek(std::size_t ahead) const {
  const std::size_t at = position_ + ahead;
  return at < input_.size() ? static_cast<unsigned char>(input_[at]) : kEof;
}

int Tokenizer::Consume() {
  if (position_ >= input_.size()) {
    ++position_;
    return kEof;
  }
  return static_cast<unsigned char>(input_[position_++]);
}

bool Tokenizer::ConsumeIfMatch(std::string_view text, bool case_insensitive) {
  if (position_ + text.size() > input_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    char a = input_[position_ + i];
    char b = text[i];
    if (case_insensitive) {
      a = ToLower(a);
      b = ToLower(b);
    }
    if (a != b) {
      return false;
    }
  }
  position_ += text.size();
  return true;
}

bool Tokenizer::AppropriateEndTag() const {
  return !last_start_tag_.empty() && current_.data == last_start_tag_;
}

void Tokenizer::FlushCharacters() {
  if (pending_characters_.empty()) {
    return;
  }
  Token token;
  token.kind = Token::Kind::Character;
  token.data = std::move(pending_characters_);
  pending_characters_.clear();
  queued_.push_back(std::move(token));
}

void Tokenizer::FinishAttribute() {
  if (!has_current_attribute_) {
    return;
  }
  // A duplicate attribute is dropped, not overwritten. The spec says the first
  // wins, and the difference is observable: `<img src=a src=b>` loads a.
  const bool duplicate =
      std::any_of(current_.attributes.begin(), current_.attributes.end(),
                  [this](const Attribute& existing) { return existing.name == current_attribute_.name; });
  if (duplicate) {
    Error();
  } else if (!current_attribute_.name.empty()) {
    current_.attributes.push_back(current_attribute_);
  }
  current_attribute_ = Attribute{};
  has_current_attribute_ = false;
}

void Tokenizer::EmitCurrent() {
  FinishAttribute();
  if (current_.kind == Token::Kind::StartTag) {
    last_start_tag_ = current_.data;
  }
  FlushCharacters();
  queued_.push_back(current_);
  current_ = Token{};
}

std::optional<Token> Tokenizer::Next() {
  while (true) {
    if (!queued_.empty()) {
      Token token = std::move(queued_.front());
      queued_.erase(queued_.begin());
      AddPerformanceCounter(PerfCounterId::HtmlTokens);
      return token;
    }
    if (emitted_eof_) {
      return std::nullopt;
    }

    switch (state_) {
      case TokenizerState::Data: {
        const int c = Consume();
        if (c == '&') {
          return_state_ = TokenizerState::Data;
          state_ = TokenizerState::CharacterReference;
        } else if (c == '<') {
          state_ = TokenizerState::TagOpen;
        } else if (c == 0) {
          Error();
          EmitCharacter('\0');
        } else if (c == kEof) {
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          EmitCharacter(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::RcData: {
        const int c = Consume();
        if (c == '&') {
          return_state_ = TokenizerState::RcData;
          state_ = TokenizerState::CharacterReference;
        } else if (c == '<') {
          state_ = TokenizerState::RcDataLessThanSign;
        } else if (c == kEof) {
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          // A NUL in RCDATA becomes U+FFFD rather than being dropped, so a
          // script cannot smuggle one past a filter that scanned the source.
          EmitCharacter(c == 0 ? '\xEF' : static_cast<char>(c));
          if (c == 0) {
            Error();
            EmitCharacter('\xBF');
            EmitCharacter('\xBD');
          }
        }
        break;
      }

      case TokenizerState::RawText:
      case TokenizerState::ScriptData: {
        const int c = Consume();
        const bool raw = state_ == TokenizerState::RawText;
        if (c == '<') {
          state_ = raw ? TokenizerState::RawTextLessThanSign
                       : TokenizerState::ScriptDataLessThanSign;
        } else if (c == kEof) {
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else if (c == 0) {
          Error();
          EmitCharacter('\xEF');
          EmitCharacter('\xBF');
          EmitCharacter('\xBD');
        } else {
          EmitCharacter(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::PlainText: {
        const int c = Consume();
        if (c == kEof) {
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          EmitCharacter(static_cast<char>(c == 0 ? ' ' : c));
        }
        break;
      }

      case TokenizerState::TagOpen: {
        const int c = Peek();
        if (c == '!') {
          ++position_;
          state_ = TokenizerState::MarkupDeclarationOpen;
        } else if (c == '/') {
          ++position_;
          state_ = TokenizerState::EndTagOpen;
        } else if (IsAsciiAlpha(c)) {
          current_ = Token{};
          current_.kind = Token::Kind::StartTag;
          state_ = TokenizerState::TagName;
        } else if (c == '?') {
          Error();
          current_ = Token{};
          current_.kind = Token::Kind::Comment;
          state_ = TokenizerState::BogusComment;
        } else if (c == kEof) {
          Error();
          EmitCharacter('<');
          state_ = TokenizerState::Data;
        } else {
          Error();
          EmitCharacter('<');
          state_ = TokenizerState::Data;
        }
        break;
      }

      case TokenizerState::EndTagOpen: {
        const int c = Peek();
        if (IsAsciiAlpha(c)) {
          current_ = Token{};
          current_.kind = Token::Kind::EndTag;
          state_ = TokenizerState::TagName;
        } else if (c == '>') {
          Error();
          ++position_;
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          EmitCharacter('<');
          EmitCharacter('/');
          state_ = TokenizerState::Data;
        } else {
          Error();
          current_ = Token{};
          current_.kind = Token::Kind::Comment;
          state_ = TokenizerState::BogusComment;
        }
        break;
      }

      case TokenizerState::TagName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeAttributeName;
        } else if (c == '/') {
          state_ = TokenizerState::SelfClosingStartTag;
        } else if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back(ToLower(static_cast<char>(c == 0 ? 0xFFFD : c)));
        }
        break;
      }

      // The three "less than sign" families are structurally identical, so they
      // share one implementation parameterized by which mode returns.
      case TokenizerState::RcDataLessThanSign:
      case TokenizerState::RawTextLessThanSign:
      case TokenizerState::ScriptDataLessThanSign: {
        const TokenizerState text_state = state_ == TokenizerState::RcDataLessThanSign
                                              ? TokenizerState::RcData
                                              : (state_ == TokenizerState::RawTextLessThanSign
                                                     ? TokenizerState::RawText
                                                     : TokenizerState::ScriptData);
        if (Peek() == '/') {
          ++position_;
          temporary_buffer_.clear();
          state_ = state_ == TokenizerState::RcDataLessThanSign
                       ? TokenizerState::RcDataEndTagOpen
                       : (state_ == TokenizerState::RawTextLessThanSign
                              ? TokenizerState::RawTextEndTagOpen
                              : TokenizerState::ScriptDataEndTagOpen);
        } else {
          EmitCharacter('<');
          state_ = text_state;
        }
        break;
      }

      case TokenizerState::RcDataEndTagOpen:
      case TokenizerState::RawTextEndTagOpen:
      case TokenizerState::ScriptDataEndTagOpen: {
        const bool rcdata = state_ == TokenizerState::RcDataEndTagOpen;
        const bool rawtext = state_ == TokenizerState::RawTextEndTagOpen;
        if (IsAsciiAlpha(Peek())) {
          current_ = Token{};
          current_.kind = Token::Kind::EndTag;
          state_ = rcdata ? TokenizerState::RcDataEndTagName
                          : (rawtext ? TokenizerState::RawTextEndTagName
                                     : TokenizerState::ScriptDataEndTagName);
        } else {
          EmitCharacter('<');
          EmitCharacter('/');
          state_ = rcdata ? TokenizerState::RcData
                          : (rawtext ? TokenizerState::RawText : TokenizerState::ScriptData);
        }
        break;
      }

      case TokenizerState::RcDataEndTagName:
      case TokenizerState::RawTextEndTagName:
      case TokenizerState::ScriptDataEndTagName: {
        const bool rcdata = state_ == TokenizerState::RcDataEndTagName;
        const bool rawtext = state_ == TokenizerState::RawTextEndTagName;
        const TokenizerState text_state =
            rcdata ? TokenizerState::RcData
                   : (rawtext ? TokenizerState::RawText : TokenizerState::ScriptData);
        const int c = Consume();
        if (IsWhitespace(c) && AppropriateEndTag()) {
          state_ = TokenizerState::BeforeAttributeName;
        } else if (c == '/' && AppropriateEndTag()) {
          state_ = TokenizerState::SelfClosingStartTag;
        } else if (c == '>' && AppropriateEndTag()) {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (IsAsciiAlpha(c)) {
          current_.data.push_back(ToLower(static_cast<char>(c)));
          temporary_buffer_.push_back(static_cast<char>(c));
        } else {
          // Not the element that opened the mode, so it was never a tag: emit
          // it as text. `</b>` inside `<title>` is the word "</b>".
          EmitCharacter('<');
          EmitCharacter('/');
          for (const char buffered : temporary_buffer_) {
            EmitCharacter(buffered);
          }
          temporary_buffer_.clear();
          current_ = Token{};
          Reconsume();
          state_ = text_state;
        }
        break;
      }

      case TokenizerState::BeforeAttributeName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        if (c == '/' || c == '>' || c == kEof) {
          Reconsume();
          state_ = TokenizerState::AfterAttributeName;
          break;
        }
        if (c == '=') {
          Error();
          FinishAttribute();
          current_attribute_.name.push_back('=');
          has_current_attribute_ = true;
          state_ = TokenizerState::AttributeName;
          break;
        }
        FinishAttribute();
        has_current_attribute_ = true;
        Reconsume();
        state_ = TokenizerState::AttributeName;
        break;
      }

      case TokenizerState::AttributeName: {
        const int c = Consume();
        if (IsWhitespace(c) || c == '/' || c == '>' || c == kEof) {
          Reconsume();
          state_ = TokenizerState::AfterAttributeName;
        } else if (c == '=') {
          state_ = TokenizerState::BeforeAttributeValue;
        } else if (c == '"' || c == '\'' || c == '<') {
          Error();
          current_attribute_.name.push_back(static_cast<char>(c));
        } else {
          current_attribute_.name.push_back(ToLower(static_cast<char>(c)));
        }
        break;
      }

      case TokenizerState::AfterAttributeName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        if (c == '/') {
          state_ = TokenizerState::SelfClosingStartTag;
        } else if (c == '=') {
          state_ = TokenizerState::BeforeAttributeValue;
        } else if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          FinishAttribute();
          has_current_attribute_ = true;
          Reconsume();
          state_ = TokenizerState::AttributeName;
        }
        break;
      }

      case TokenizerState::BeforeAttributeValue: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        if (c == '"') {
          state_ = TokenizerState::AttributeValueDoubleQuoted;
        } else if (c == '\'') {
          state_ = TokenizerState::AttributeValueSingleQuoted;
        } else if (c == '>') {
          Error();
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else {
          Reconsume();
          state_ = TokenizerState::AttributeValueUnquoted;
        }
        break;
      }

      case TokenizerState::AttributeValueDoubleQuoted:
      case TokenizerState::AttributeValueSingleQuoted: {
        const char quote = state_ == TokenizerState::AttributeValueDoubleQuoted ? '"' : '\'';
        const int c = Consume();
        if (c == quote) {
          state_ = TokenizerState::AfterAttributeValueQuoted;
        } else if (c == '&') {
          return_state_ = state_;
          state_ = TokenizerState::CharacterReference;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_attribute_.value.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::AttributeValueUnquoted: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeAttributeName;
        } else if (c == '&') {
          return_state_ = TokenizerState::AttributeValueUnquoted;
          state_ = TokenizerState::CharacterReference;
        } else if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') {
            Error();
          }
          current_attribute_.value.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::AfterAttributeValueQuoted: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeAttributeName;
        } else if (c == '/') {
          state_ = TokenizerState::SelfClosingStartTag;
        } else if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          Error();
          Reconsume();
          state_ = TokenizerState::BeforeAttributeName;
        }
        break;
      }

      case TokenizerState::SelfClosingStartTag: {
        const int c = Consume();
        if (c == '>') {
          current_.self_closing = true;
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          Error();
          Reconsume();
          state_ = TokenizerState::BeforeAttributeName;
        }
        break;
      }

      case TokenizerState::BogusComment: {
        const int c = Consume();
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::MarkupDeclarationOpen: {
        if (ConsumeIfMatch("--", false)) {
          current_ = Token{};
          current_.kind = Token::Kind::Comment;
          state_ = TokenizerState::CommentStart;
        } else if (ConsumeIfMatch("DOCTYPE", true)) {
          state_ = TokenizerState::Doctype;
        } else if (ConsumeIfMatch("[CDATA[", false)) {
          // No foreign content yet, so this is a bogus comment. The spec's
          // other branch needs an adjusted current node from the tree builder.
          Error();
          current_ = Token{};
          current_.kind = Token::Kind::Comment;
          state_ = TokenizerState::BogusComment;
        } else {
          Error();
          current_ = Token{};
          current_.kind = Token::Kind::Comment;
          state_ = TokenizerState::BogusComment;
        }
        break;
      }

      case TokenizerState::CommentStart: {
        const int c = Peek();
        if (c == '-') {
          ++position_;
          state_ = TokenizerState::CommentStartDash;
        } else if (c == '>') {
          ++position_;
          Error();
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else {
          state_ = TokenizerState::Comment;
        }
        break;
      }

      case TokenizerState::CommentStartDash: {
        const int c = Consume();
        if (c == '-') {
          state_ = TokenizerState::CommentEnd;
        } else if (c == '>') {
          Error();
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back('-');
          Reconsume();
          state_ = TokenizerState::Comment;
        }
        break;
      }

      case TokenizerState::Comment: {
        const int c = Consume();
        if (c == '-') {
          state_ = TokenizerState::CommentEndDash;
        } else if (c == kEof) {
          Error();
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::CommentEndDash: {
        const int c = Consume();
        if (c == '-') {
          state_ = TokenizerState::CommentEnd;
        } else if (c == kEof) {
          Error();
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back('-');
          Reconsume();
          state_ = TokenizerState::Comment;
        }
        break;
      }

      case TokenizerState::CommentEnd: {
        const int c = Consume();
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == '!') {
          state_ = TokenizerState::CommentEndBang;
        } else if (c == '-') {
          current_.data.push_back('-');
        } else if (c == kEof) {
          Error();
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.append("--");
          Reconsume();
          state_ = TokenizerState::Comment;
        }
        break;
      }

      case TokenizerState::CommentEndBang: {
        const int c = Consume();
        if (c == '-') {
          current_.data.append("--!");
          state_ = TokenizerState::CommentEndDash;
        } else if (c == '>') {
          Error();
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.append("--!");
          Reconsume();
          state_ = TokenizerState::Comment;
        }
        break;
      }

      case TokenizerState::Doctype: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeDoctypeName;
        } else if (c == kEof) {
          Error();
          current_ = Token{};
          current_.kind = Token::Kind::Doctype;
          current_.force_quirks = true;
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          Error();
          Reconsume();
          state_ = TokenizerState::BeforeDoctypeName;
        }
        break;
      }

      case TokenizerState::BeforeDoctypeName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        current_ = Token{};
        current_.kind = Token::Kind::Doctype;
        if (c == '>') {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back(ToLower(static_cast<char>(c)));
          state_ = TokenizerState::DoctypeName;
        }
        break;
      }

      case TokenizerState::DoctypeName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::AfterDoctypeName;
        } else if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          current_.data.push_back(ToLower(static_cast<char>(c)));
        }
        break;
      }

      case TokenizerState::AfterDoctypeName: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          Reconsume();
          if (ConsumeIfMatch("PUBLIC", true)) {
            state_ = TokenizerState::AfterDoctypePublicKeyword;
          } else if (ConsumeIfMatch("SYSTEM", true)) {
            state_ = TokenizerState::AfterDoctypeSystemKeyword;
          } else {
            Error();
            current_.force_quirks = true;
            ++position_;
            state_ = TokenizerState::BogusDoctype;
          }
        }
        break;
      }

      case TokenizerState::AfterDoctypePublicKeyword:
      case TokenizerState::BeforeDoctypePublicIdentifier: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeDoctypePublicIdentifier;
          break;
        }
        if (c == '"' || c == '\'') {
          current_.has_public_identifier = true;
          state_ = c == '"' ? TokenizerState::DoctypePublicIdentifierDoubleQuoted
                            : TokenizerState::DoctypePublicIdentifierSingleQuoted;
        } else {
          Error();
          current_.force_quirks = true;
          Reconsume();
          state_ = TokenizerState::BogusDoctype;
        }
        break;
      }

      case TokenizerState::DoctypePublicIdentifierDoubleQuoted:
      case TokenizerState::DoctypePublicIdentifierSingleQuoted: {
        const char quote =
            state_ == TokenizerState::DoctypePublicIdentifierDoubleQuoted ? '"' : '\'';
        const int c = Consume();
        if (c == quote) {
          state_ = TokenizerState::AfterDoctypePublicIdentifier;
        } else if (c == '>' || c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          state_ = TokenizerState::Data;
          if (c == kEof) {
            FlushCharacters();
            Token eof;
            eof.kind = Token::Kind::EndOfFile;
            queued_.push_back(eof);
            emitted_eof_ = true;
          }
        } else {
          current_.public_identifier.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::AfterDoctypePublicIdentifier:
      case TokenizerState::BetweenDoctypePublicAndSystemIdentifiers: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BetweenDoctypePublicAndSystemIdentifiers;
          break;
        }
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == '"' || c == '\'') {
          current_.has_system_identifier = true;
          state_ = c == '"' ? TokenizerState::DoctypeSystemIdentifierDoubleQuoted
                            : TokenizerState::DoctypeSystemIdentifierSingleQuoted;
        } else {
          Error();
          current_.force_quirks = true;
          Reconsume();
          state_ = TokenizerState::BogusDoctype;
        }
        break;
      }

      case TokenizerState::AfterDoctypeSystemKeyword:
      case TokenizerState::BeforeDoctypeSystemIdentifier: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          state_ = TokenizerState::BeforeDoctypeSystemIdentifier;
          break;
        }
        if (c == '"' || c == '\'') {
          current_.has_system_identifier = true;
          state_ = c == '"' ? TokenizerState::DoctypeSystemIdentifierDoubleQuoted
                            : TokenizerState::DoctypeSystemIdentifierSingleQuoted;
        } else {
          Error();
          current_.force_quirks = true;
          Reconsume();
          state_ = TokenizerState::BogusDoctype;
        }
        break;
      }

      case TokenizerState::DoctypeSystemIdentifierDoubleQuoted:
      case TokenizerState::DoctypeSystemIdentifierSingleQuoted: {
        const char quote =
            state_ == TokenizerState::DoctypeSystemIdentifierDoubleQuoted ? '"' : '\'';
        const int c = Consume();
        if (c == quote) {
          state_ = TokenizerState::AfterDoctypeSystemIdentifier;
        } else if (c == '>' || c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          state_ = TokenizerState::Data;
          if (c == kEof) {
            FlushCharacters();
            Token eof;
            eof.kind = Token::Kind::EndOfFile;
            queued_.push_back(eof);
            emitted_eof_ = true;
          }
        } else {
          current_.system_identifier.push_back(static_cast<char>(c));
        }
        break;
      }

      case TokenizerState::AfterDoctypeSystemIdentifier: {
        const int c = Consume();
        if (IsWhitespace(c)) {
          break;
        }
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          Error();
          current_.force_quirks = true;
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        } else {
          Error();
          Reconsume();
          state_ = TokenizerState::BogusDoctype;
        }
        break;
      }

      case TokenizerState::BogusDoctype: {
        const int c = Consume();
        if (c == '>') {
          EmitCurrent();
          state_ = TokenizerState::Data;
        } else if (c == kEof) {
          EmitCurrent();
          FlushCharacters();
          Token eof;
          eof.kind = Token::Kind::EndOfFile;
          queued_.push_back(eof);
          emitted_eof_ = true;
        }
        break;
      }

      case TokenizerState::CharacterReference: {
        temporary_buffer_.assign("&");
        const int c = Peek();
        if (IsAsciiAlphanumeric(c)) {
          state_ = TokenizerState::NamedCharacterReference;
        } else if (c == '#') {
          ++position_;
          temporary_buffer_.push_back('#');
          state_ = TokenizerState::NumericCharacterReference;
        } else {
          // Flush and go back: a bare `&` is a literal ampersand.
          const bool in_attribute = return_state_ == TokenizerState::AttributeValueDoubleQuoted ||
                                    return_state_ == TokenizerState::AttributeValueSingleQuoted ||
                                    return_state_ == TokenizerState::AttributeValueUnquoted;
          if (in_attribute) {
            current_attribute_.value += temporary_buffer_;
          } else {
            for (const char buffered : temporary_buffer_) {
              EmitCharacter(buffered);
            }
          }
          state_ = return_state_;
        }
        break;
      }

      case TokenizerState::NamedCharacterReference: {
        const NamedReference match = LookUpNamedCharacterReference(input_.substr(position_));
        const bool in_attribute = return_state_ == TokenizerState::AttributeValueDoubleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueSingleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueUnquoted;
        if (match.consumed > 0) {
          const bool terminated = input_[position_ + match.consumed - 1] == ';';
          // The legacy attribute rule: inside an attribute, an unterminated
          // reference followed by `=` or an alphanumeric is *not* expanded, so
          // that `?a&copy=1` keeps its query parameter instead of becoming
          // `?a©=1`. Real URLs depend on this.
          const int after = position_ + match.consumed < input_.size()
                                ? static_cast<unsigned char>(input_[position_ + match.consumed])
                                : kEof;
          if (in_attribute && !terminated && (after == '=' || IsAsciiAlphanumeric(after))) {
            for (std::size_t i = 0; i < match.consumed; ++i) {
              temporary_buffer_.push_back(input_[position_ + i]);
            }
            position_ += match.consumed;
            current_attribute_.value += temporary_buffer_;
            state_ = return_state_;
            break;
          }
          if (!terminated) {
            Error();
          }
          position_ += match.consumed;
          if (in_attribute) {
            current_attribute_.value += match.replacement;
          } else {
            for (const char produced : match.replacement) {
              EmitCharacter(produced);
            }
          }
          state_ = return_state_;
        } else {
          if (in_attribute) {
            current_attribute_.value += temporary_buffer_;
          } else {
            for (const char buffered : temporary_buffer_) {
              EmitCharacter(buffered);
            }
          }
          state_ = TokenizerState::AmbiguousAmpersand;
        }
        break;
      }

      case TokenizerState::AmbiguousAmpersand: {
        const int c = Peek();
        const bool in_attribute = return_state_ == TokenizerState::AttributeValueDoubleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueSingleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueUnquoted;
        if (IsAsciiAlphanumeric(c)) {
          ++position_;
          if (in_attribute) {
            current_attribute_.value.push_back(static_cast<char>(c));
          } else {
            EmitCharacter(static_cast<char>(c));
          }
        } else {
          if (c == ';') {
            Error();
          }
          state_ = return_state_;
        }
        break;
      }

      case TokenizerState::NumericCharacterReference: {
        character_reference_code_ = 0;
        const int c = Peek();
        if (c == 'x' || c == 'X') {
          ++position_;
          temporary_buffer_.push_back(static_cast<char>(c));
          state_ = TokenizerState::HexadecimalCharacterReferenceStart;
        } else {
          state_ = TokenizerState::DecimalCharacterReferenceStart;
        }
        break;
      }

      case TokenizerState::HexadecimalCharacterReferenceStart:
      case TokenizerState::DecimalCharacterReferenceStart: {
        const bool hex = state_ == TokenizerState::HexadecimalCharacterReferenceStart;
        const int c = Peek();
        const bool digit = hex ? (IsAsciiAlphanumeric(c) &&
                                  (std::isdigit(c) != 0 || ToLower(static_cast<char>(c)) <= 'f'))
                               : (c >= '0' && c <= '9');
        if (digit) {
          state_ = hex ? TokenizerState::HexadecimalCharacterReference
                       : TokenizerState::DecimalCharacterReference;
        } else {
          Error();
          const bool in_attribute = return_state_ == TokenizerState::AttributeValueDoubleQuoted ||
                                    return_state_ == TokenizerState::AttributeValueSingleQuoted ||
                                    return_state_ == TokenizerState::AttributeValueUnquoted;
          if (in_attribute) {
            current_attribute_.value += temporary_buffer_;
          } else {
            for (const char buffered : temporary_buffer_) {
              EmitCharacter(buffered);
            }
          }
          state_ = return_state_;
        }
        break;
      }

      case TokenizerState::HexadecimalCharacterReference:
      case TokenizerState::DecimalCharacterReference: {
        const bool hex = state_ == TokenizerState::HexadecimalCharacterReference;
        const int c = Consume();
        int value = -1;
        if (c >= '0' && c <= '9') {
          value = c - '0';
        } else if (hex && c >= 'a' && c <= 'f') {
          value = c - 'a' + 10;
        } else if (hex && c >= 'A' && c <= 'F') {
          value = c - 'A' + 10;
        }

        if (value >= 0) {
          // Saturating rather than wrapping. A reference of a thousand digits
          // must not wrap into a plausible code point.
          if (character_reference_code_ < 0x110000) {
            character_reference_code_ =
                character_reference_code_ * (hex ? 16u : 10u) + static_cast<std::uint32_t>(value);
          }
        } else if (c == ';') {
          state_ = TokenizerState::NumericCharacterReferenceEnd;
        } else {
          Error();
          Reconsume();
          state_ = TokenizerState::NumericCharacterReferenceEnd;
        }
        break;
      }

      case TokenizerState::NumericCharacterReferenceEnd: {
        std::uint32_t code = character_reference_code_;
        if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
          // Null, out of range, and surrogates all become U+FFFD. A surrogate
          // passed through would be invalid UTF-8 in the DOM.
          Error();
          code = 0xFFFD;
        } else if (code >= 0x80 && code <= 0x9F) {
          Error();
          code = kC1Replacements[code - 0x80];
        }
        std::string produced;
        AppendUtf8(code, produced);

        const bool in_attribute = return_state_ == TokenizerState::AttributeValueDoubleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueSingleQuoted ||
                                  return_state_ == TokenizerState::AttributeValueUnquoted;
        if (in_attribute) {
          current_attribute_.value += produced;
        } else {
          for (const char c : produced) {
            EmitCharacter(c);
          }
        }
        state_ = return_state_;
        break;
      }

      case TokenizerState::CommentLessThanSign:
        state_ = TokenizerState::Comment;
        break;
    }
  }
}

}  // namespace microbrowser::html
