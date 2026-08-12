#pragma once

// A minimal JSON reader for the conformance runners in this directory.
//
// It is here rather than in `src/util` on purpose: nothing this browser ships
// parses JSON in C++ -- `src/js` owns the one JSON implementation, because the
// only JSON a page produces is a page's -- and a second one in the shipping
// tree would be a second answer to the same question. A tool that reads a
// checked-in test vector is not that; it reads files this repository pins.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace microbrowser::urlconf {

class JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<JsonPtr> array;
  std::map<std::string, JsonPtr> object;

  bool IsNull() const { return kind == Kind::Null; }
  bool IsString() const { return kind == Kind::String; }
  bool IsObject() const { return kind == Kind::Object; }
  bool IsArray() const { return kind == Kind::Array; }

  // Nullptr when absent, which every caller here treats as "the test does not
  // constrain this field" rather than as an empty expectation.
  const JsonValue* Find(std::string_view key) const {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : it->second.get();
  }
  bool Truthy(std::string_view key) const {
    const JsonValue* found = Find(key);
    return found != nullptr && found->kind == Kind::Bool && found->boolean;
  }
  std::optional<std::string> Str(std::string_view key) const {
    const JsonValue* found = Find(key);
    if (found == nullptr || !found->IsString()) {
      return std::nullopt;
    }
    return found->string;
  }
};

// Parses `text`. Returns nullptr on malformed input; these files are pinned, so
// a failure here is a broken checkout rather than something to recover from.
JsonPtr ParseJson(std::string_view text);

// Reads a whole file, or nullopt when it is not there.
std::optional<std::string> ReadFile(const std::string& path);

}  // namespace microbrowser::urlconf
