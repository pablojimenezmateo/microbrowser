#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js/BuiltinSupport.h"
#include "js/Interpreter.h"
#include "util/Parse.h"

// `JSON.parse`, and the URI encoding functions beside it.
//
// Both take text a page hands over, and `JSON.parse` in particular takes text
// that usually came off the network -- so the parser is written the way the
// HTML and CSS ones are: every read bounds-checked, the nesting depth bounded
// so a wall of `[` is a SyntaxError rather than a stack overflow, and the
// result size bounded so a small input cannot ask for a large allocation.
//
// `JSON.stringify` lives in Builtins.cpp with the rest of the object; only
// what did not exist is here.

namespace microbrowser::js {

namespace {

// Deep enough for any document and shallow enough that the recursion below
// cannot reach the bottom of the C++ stack. A page can serve `[[[[[...`.
constexpr int kMaxJsonDepth = 200;

// One parse, over one buffer.
class JsonParser {
 public:
  JsonParser(std::string_view text, Interpreter& interpreter)
      : text_(text), interpreter_(interpreter) {}

  // Null when the text is not JSON, with `error` describing where.
  bool Parse(Value& out) {
    SkipWhitespace();
    if (!ParseValue(out, 0)) {
      return false;
    }
    SkipWhitespace();
    // Trailing content is an error rather than ignored: `{"a":1}garbage` is
    // not a document, and accepting the prefix is how a truncated response
    // gets treated as a whole one.
    if (at_ != text_.size()) {
      error_ = "unexpected trailing content";
      return false;
    }
    return true;
  }

  const std::string& Error() const { return error_; }

 private:
  bool AtEnd() const { return at_ >= text_.size(); }
  char Peek() const { return AtEnd() ? '\0' : text_[at_]; }

  void SkipWhitespace() {
    while (!AtEnd() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' ||
                        text_[at_] == '\r')) {
      ++at_;
    }
  }

  bool Literal(std::string_view word, Value value, Value& out) {
    if (text_.compare(at_, word.size(), word) != 0) {
      error_ = "unexpected token";
      return false;
    }
    at_ += word.size();
    out = std::move(value);
    return true;
  }

  bool ParseValue(Value& out, int depth) {
    if (depth > kMaxJsonDepth) {
      error_ = "nested too deeply";
      return false;
    }
    SkipWhitespace();
    if (AtEnd()) {
      error_ = "unexpected end of input";
      return false;
    }
    switch (Peek()) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"': {
        std::string text;
        if (!ParseString(text)) {
          return false;
        }
        out = Value::String(std::move(text));
        return true;
      }
      case 't': return Literal("true", Value::Bool(true), out);
      case 'f': return Literal("false", Value::Bool(false), out);
      case 'n': return Literal("null", Value::Null(), out);
      default: break;
    }
    return ParseNumber(out);
  }

  bool ParseObject(Value& out, int depth) {
    ++at_;  // '{'
    const Value object = interpreter_.NewObjectValue();
    if (!object.IsObject()) {
      error_ = "out of memory";
      return false;
    }
    SkipWhitespace();
    if (Peek() == '}') {
      ++at_;
      out = object;
      return true;
    }
    for (;;) {
      SkipWhitespace();
      std::string key;
      if (Peek() != '"' || !ParseString(key)) {
        error_ = "expected a property name";
        return false;
      }
      SkipWhitespace();
      if (Peek() != ':') {
        error_ = "expected ':' after a property name";
        return false;
      }
      ++at_;
      Value value;
      if (!ParseValue(value, depth + 1)) {
        return false;
      }
      object.object->Set(key, value);
      SkipWhitespace();
      if (Peek() == ',') {
        ++at_;
        continue;
      }
      if (Peek() == '}') {
        ++at_;
        out = object;
        return true;
      }
      error_ = "expected ',' or '}'";
      return false;
    }
  }

  bool ParseArray(Value& out, int depth) {
    ++at_;  // '['
    std::vector<Value> elements;
    SkipWhitespace();
    if (Peek() == ']') {
      ++at_;
      out = interpreter_.NewArrayValue(std::move(elements));
      return true;
    }
    for (;;) {
      Value value;
      if (!ParseValue(value, depth + 1)) {
        return false;
      }
      if (elements.size() >= kMaxAllocationLength) {
        error_ = "array is too long";
        return false;
      }
      elements.push_back(std::move(value));
      SkipWhitespace();
      if (Peek() == ',') {
        ++at_;
        continue;
      }
      if (Peek() == ']') {
        ++at_;
        out = interpreter_.NewArrayValue(std::move(elements));
        return true;
      }
      error_ = "expected ',' or ']'";
      return false;
    }
  }

  // A JSON string, with its escapes. `\u` is decoded to UTF-8 rather than kept
  // as a code unit, because a string here is bytes -- see the note on
  // String.prototype.
  bool ParseString(std::string& out) {
    ++at_;  // the opening quote
    out.clear();
    while (!AtEnd()) {
      const char c = text_[at_];
      if (c == '"') {
        ++at_;
        return true;
      }
      if (c != '\\') {
        // A raw control character is invalid JSON. Accepted anyway would mean
        // a document with an embedded newline round-trips differently than it
        // parsed.
        if (static_cast<unsigned char>(c) < 0x20) {
          error_ = "a control character must be escaped";
          return false;
        }
        out.push_back(c);
        ++at_;
        continue;
      }
      ++at_;
      if (AtEnd()) {
        break;
      }
      const char escape = text_[at_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned int code = 0;
          if (!ReadHex4(code)) {
            return false;
          }
          // A surrogate pair is one character, and it arrives as two escapes.
          if (code >= 0xD800 && code <= 0xDBFF && at_ + 1 < text_.size() &&
              text_[at_] == '\\' && text_[at_ + 1] == 'u') {
            const std::size_t saved = at_;
            at_ += 2;
            unsigned int low = 0;
            if (ReadHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else {
              at_ = saved;
            }
          }
          AppendUtf8(out, code);
          break;
        }
        default:
          error_ = "unrecognized escape";
          return false;
      }
    }
    error_ = "unterminated string";
    return false;
  }

  bool ReadHex4(unsigned int& code) {
    if (at_ + 4 > text_.size()) {
      error_ = "truncated \\u escape";
      return false;
    }
    code = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = text_[at_ + i];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        error_ = "invalid \\u escape";
        return false;
      }
      code = code * 16 + static_cast<unsigned int>(digit);
    }
    at_ += 4;
    return true;
  }

  static void AppendUtf8(std::string& out, unsigned int code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool ParseNumber(Value& out) {
    const std::size_t start = at_;
    if (!AtEnd() && text_[at_] == '-') {
      ++at_;
    }
    while (!AtEnd() && text_[at_] >= '0' && text_[at_] <= '9') {
      ++at_;
    }
    if (!AtEnd() && text_[at_] == '.') {
      ++at_;
      while (!AtEnd() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
      }
    }
    if (!AtEnd() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      ++at_;
      if (!AtEnd() && (text_[at_] == '+' || text_[at_] == '-')) {
        ++at_;
      }
      while (!AtEnd() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
      }
    }
    const std::string_view number = text_.substr(start, at_ - start);
    // Through util::ParseDouble rather than strtod: the standard conversions
    // read the decimal separator from the process locale, which SDL changes
    // behind our back.
    const std::optional<double> parsed = util::ParseDouble(number);
    if (number.empty() || !parsed.has_value()) {
      error_ = "expected a number";
      at_ = start;
      return false;
    }
    out = Value::Number(*parsed);
    return true;
  }

  std::string_view text_;
  Interpreter& interpreter_;
  std::size_t at_ = 0;
  std::string error_;
};

// The characters each URI function leaves alone. The two differ, and the
// difference is the whole reason both exist: `encodeURI` keeps the punctuation
// that separates the parts of a URL, `encodeURIComponent` escapes it so a
// value cannot become a separator.
bool IsUnreserved(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
         c == '(' || c == ')';
}

bool IsUriReserved(char c) {
  return c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' || c == '=' ||
         c == '+' || c == '$' || c == ',' || c == '#';
}

std::string PercentEncode(std::string_view text, bool keep_reserved) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (IsUnreserved(c) || (keep_reserved && IsUriReserved(c))) {
      out.push_back(c);
      continue;
    }
    const auto byte = static_cast<unsigned char>(c);
    out.push_back('%');
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// False when a `%` is not followed by two hex digits, which the spec makes a
// URIError rather than a passthrough.
bool PercentDecode(std::string_view text, bool keep_reserved, std::string& out) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return false;
    }
    const int high = HexDigit(text[i + 1]);
    const int low = HexDigit(text[i + 2]);
    if (high < 0 || low < 0) {
      return false;
    }
    const auto byte = static_cast<char>(high * 16 + low);
    i += 2;
    // `decodeURI` leaves the separators encoded, so that decoding a URL cannot
    // turn a value into a new component.
    out += keep_reserved && IsUriReserved(byte) ? std::string{'%', text[i - 1], text[i]}
                                                : std::string(1, byte);
  }
  return true;
}

}  // namespace

void Interpreter::InstallJsonAndUri(Object* json) {
  InstallNative(json, "parse", [](NativeCall& call) {
    const std::string text = ToString(Argument(call.arguments, 0));
    JsonParser parser(text, call.interpreter);
    Value parsed;
    if (!parser.Parse(parsed)) {
      return call.Throw("SyntaxError", "invalid JSON: " + parser.Error());
    }
    // The reviver is not implemented. A page that passes one gets its data
    // unrevived rather than nothing, which is the failure it is most likely to
    // survive.
    return parsed;
  });

  const auto uri = [this](const char* name, bool encode, bool keep_reserved) {
    global_scope_->Declare(
        name,
        NewNativeValue(name,
                       [encode, keep_reserved](NativeCall& call) {
                         const std::string text = ToString(Argument(call.arguments, 0));
                         if (encode) {
                           return Value::String(PercentEncode(text, keep_reserved));
                         }
                         std::string decoded;
                         if (!PercentDecode(text, keep_reserved, decoded)) {
                           return call.Throw("URIError", "malformed URI sequence");
                         }
                         return Value::String(std::move(decoded));
                       }),
        false);
  };
  uri("encodeURIComponent", true, false);
  uri("encodeURI", true, true);
  uri("decodeURIComponent", false, false);
  uri("decodeURI", false, true);
}

}  // namespace microbrowser::js
