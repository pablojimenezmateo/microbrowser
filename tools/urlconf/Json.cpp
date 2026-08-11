#include "urlconf/Json.h"

#include <cstdio>
#include <cstdlib>

namespace microbrowser::urlconf {

namespace {

// Appends one code point as UTF-8. A lone surrogate becomes U+FFFD, which is
// not a shortcut: the APIs under test take a USVString, and converting a
// DOMString to one replaces exactly these.
void AppendUtf8(std::string& out, std::uint32_t code_point) {
  if (code_point >= 0xD800 && code_point <= 0xDFFF) {
    code_point = 0xFFFD;
  }
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
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

class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  JsonPtr ParseValue() {
    SkipSpace();
    if (position_ >= text_.size()) {
      return nullptr;
    }
    switch (text_[position_]) {
      case '{':
        return ParseObject();
      case '[':
        return ParseArray();
      case '"':
        return ParseString();
      case 't':
      case 'f':
        return ParseBool();
      case 'n':
        return ParseNull();
      default:
        return ParseNumber();
    }
  }

 private:
  void SkipSpace() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
            text_[position_] == '\r')) {
      ++position_;
    }
  }

  bool Eat(char c) {
    SkipSpace();
    if (position_ < text_.size() && text_[position_] == c) {
      ++position_;
      return true;
    }
    return false;
  }

  JsonPtr ParseObject() {
    ++position_;
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValue::Kind::Object;
    SkipSpace();
    if (Eat('}')) {
      return value;
    }
    while (true) {
      SkipSpace();
      const JsonPtr key = ParseString();
      if (key == nullptr || !Eat(':')) {
        return nullptr;
      }
      const JsonPtr member = ParseValue();
      if (member == nullptr) {
        return nullptr;
      }
      value->object[key->string] = member;
      if (Eat(',')) {
        continue;
      }
      return Eat('}') ? value : nullptr;
    }
  }

  JsonPtr ParseArray() {
    ++position_;
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValue::Kind::Array;
    SkipSpace();
    if (Eat(']')) {
      return value;
    }
    while (true) {
      const JsonPtr element = ParseValue();
      if (element == nullptr) {
        return nullptr;
      }
      value->array.push_back(element);
      if (Eat(',')) {
        continue;
      }
      return Eat(']') ? value : nullptr;
    }
  }

  JsonPtr ParseString() {
    SkipSpace();
    if (position_ >= text_.size() || text_[position_] != '"') {
      return nullptr;
    }
    ++position_;
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValue::Kind::String;
    while (position_ < text_.size() && text_[position_] != '"') {
      const char c = text_[position_];
      if (c != '\\') {
        value->string.push_back(c);
        ++position_;
        continue;
      }
      ++position_;
      if (position_ >= text_.size()) {
        return nullptr;
      }
      const char escape = text_[position_++];
      switch (escape) {
        case 'n': value->string.push_back('\n'); break;
        case 't': value->string.push_back('\t'); break;
        case 'r': value->string.push_back('\r'); break;
        case 'b': value->string.push_back('\b'); break;
        case 'f': value->string.push_back('\f'); break;
        case '/': value->string.push_back('/'); break;
        case '\\': value->string.push_back('\\'); break;
        case '"': value->string.push_back('"'); break;
        case 'u': {
          const std::optional<std::uint32_t> first = ParseHex4();
          if (!first.has_value()) {
            return nullptr;
          }
          std::uint32_t code_point = *first;
          // A surrogate pair is two escapes; only a pair becomes one code
          // point, and a high surrogate followed by anything else is lone.
          if (code_point >= 0xD800 && code_point <= 0xDBFF && position_ + 1 < text_.size() &&
              text_[position_] == '\\' && text_[position_ + 1] == 'u') {
            const std::size_t saved = position_;
            position_ += 2;
            const std::optional<std::uint32_t> second = ParseHex4();
            if (second.has_value() && *second >= 0xDC00 && *second <= 0xDFFF) {
              code_point = 0x10000 + ((code_point - 0xD800) << 10) + (*second - 0xDC00);
            } else {
              position_ = saved;
            }
          }
          AppendUtf8(value->string, code_point);
          break;
        }
        default:
          return nullptr;
      }
    }
    if (position_ >= text_.size()) {
      return nullptr;
    }
    ++position_;
    return value;
  }

  std::optional<std::uint32_t> ParseHex4() {
    if (position_ + 4 > text_.size()) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    return value;
  }

  JsonPtr ParseBool() {
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValue::Kind::Bool;
    if (text_.compare(position_, 4, "true") == 0) {
      value->boolean = true;
      position_ += 4;
      return value;
    }
    if (text_.compare(position_, 5, "false") == 0) {
      value->boolean = false;
      position_ += 5;
      return value;
    }
    return nullptr;
  }

  JsonPtr ParseNull() {
    if (text_.compare(position_, 4, "null") != 0) {
      return nullptr;
    }
    position_ += 4;
    return std::make_shared<JsonValue>();
  }

  JsonPtr ParseNumber() {
    const std::size_t start = position_;
    while (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+' ||
                                        text_[position_] == '.' || text_[position_] == 'e' ||
                                        text_[position_] == 'E' ||
                                        (text_[position_] >= '0' && text_[position_] <= '9'))) {
      ++position_;
    }
    if (position_ == start) {
      return nullptr;
    }
    auto value = std::make_shared<JsonValue>();
    value->kind = JsonValue::Kind::Number;
    value->number = std::strtod(std::string(text_.substr(start, position_ - start)).c_str(), nullptr);
    return value;
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

}  // namespace

JsonPtr ParseJson(std::string_view text) {
  Reader reader(text);
  return reader.ParseValue();
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return std::nullopt;
  }
  std::string out;
  char buffer[65536];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, read);
  }
  std::fclose(file);
  return out;
}

}  // namespace microbrowser::urlconf
