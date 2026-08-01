#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace microbrowser::html {

struct Attribute {
  std::string name;
  std::string value;

  friend bool operator==(const Attribute&, const Attribute&) = default;
};

// One token from the HTML tokenizer, as §13.2.5 defines them.
//
// A tagged struct rather than a variant because a start tag and an end tag
// differ by one bool and share every field, and the tree builder switches on
// the kind constantly. Splitting them would mean two nearly identical branches
// everywhere.
struct Token {
  enum class Kind : std::uint8_t {
    Doctype,
    StartTag,
    EndTag,
    Comment,
    Character,
    EndOfFile,
  };

  Kind kind = Kind::EndOfFile;

  // Tag name, comment data, or the character run. One field for all three
  // because no token uses more than one of them.
  std::string data;
  std::vector<Attribute> attributes;

  bool self_closing = false;
  // Doctype only. The spec tracks "missing" separately from "empty", and the
  // difference decides quirks mode.
  bool force_quirks = false;
  std::string public_identifier;
  std::string system_identifier;
  bool has_public_identifier = false;
  bool has_system_identifier = false;

  const std::string* AttributeValue(std::string_view name) const;
  bool HasAttribute(std::string_view name) const { return AttributeValue(name) != nullptr; }

  friend bool operator==(const Token&, const Token&) = default;
};

}  // namespace microbrowser::html
