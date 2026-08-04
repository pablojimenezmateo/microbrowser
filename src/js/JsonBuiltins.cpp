#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
// `JSON.stringify` is here too now. It used to be a writer in Builtins.cpp
// that took one argument and understood plain data; a replacer, an indent, a
// `toJSON` and a named cycle error are all things a page actually uses, and
// they belong beside the parser rather than in the file everything lands in.

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

namespace {

// One `JSON.stringify` in progress.
//
// A class rather than a function because it carries three things every level
// needs and none of them is the value being written: the replacer, the indent,
// and the stack of objects already open. That last is what makes a cycle a
// TypeError naming the problem rather than a depth limit hit at random.
class JsonWriter {
 public:
  JsonWriter(NativeCall& call, Value replacer, std::string indent)
      : call_(call), replacer_(std::move(replacer)), indent_(std::move(indent)) {
    // A replacer array lists the keys to keep. Read once here rather than
    // searched per property per level.
    if (replacer_.IsObject() && replacer_.object->GetKind() == Object::Kind::Array) {
      for (std::size_t i = 0; i < replacer_.object->ElementCount(); ++i) {
        const Value entry = replacer_.object->GetElement(i);
        if (entry.IsString() || entry.IsNumber()) {
          allowed_.push_back(ToString(entry));
        }
      }
      filtering_ = true;
      replacer_ = Value::Undefined();
    } else if (!(replacer_.IsObject() && replacer_.object->IsCallable())) {
      replacer_ = Value::Undefined();
    }
  }

  // False when the value has no JSON form at all -- a bare `undefined`, a
  // function, a symbol -- which is what makes `JSON.stringify(undefined)`
  // undefined rather than the string "undefined".
  bool Write(const Value& holder, const std::string& key, const Value& raw, int depth,
             std::string& out) {
    Value value = raw;
    if (!Prepare(holder, key, value)) {
      return false;
    }
    switch (value.type) {
      case ValueType::Undefined:
      case ValueType::Symbol:
        return false;
      case ValueType::Null:
        out += "null";
        return true;
      case ValueType::Boolean:
        out += value.boolean ? "true" : "false";
        return true;
      case ValueType::Number:
        // A non-finite number has no JSON spelling, and the spec writes null
        // rather than refusing the whole document.
        out += std::isfinite(value.number) ? NumberToString(value.number) : "null";
        return true;
      case ValueType::String:
        WriteString(value.AsString(), out);
        return true;
      case ValueType::Object:
        break;
    }
    if (value.object->IsCallable()) {
      return false;
    }
    if (depth > kMaxJsonDepth) {
      call_.Throw("TypeError", "value nested too deeply to serialize");
      threw_ = true;
      return false;
    }
    for (const Object* open : open_) {
      if (open == value.object) {
        // Named rather than reported as depth: a cycle is a bug in the page's
        // data and "converting circular structure to JSON" is what says so.
        call_.Throw("TypeError", "converting circular structure to JSON");
        threw_ = true;
        return false;
      }
    }
    open_.push_back(value.object);
    const bool ok = value.object->GetKind() == Object::Kind::Array
                        ? WriteArray(value, depth, out)
                        : WriteObject(value, depth, out);
    open_.pop_back();
    return ok;
  }

  bool Threw() const { return threw_; }
  // The value to rethrow, when the failure came from a callback rather than
  // from this writer. A writer-raised error is already recorded on the call.
  const Result& Failure() const { return failed_; }

 private:
  // `toJSON` first, then the replacer -- in that order, which is what lets a
  // Date serialize as its ISO string and a replacer still see the string.
  bool Prepare(const Value& holder, const std::string& key, Value& value) {
    if (value.IsObject()) {
      const Value method = call_.interpreter.GetPropertyValue(value, "toJSON");
      if (method.IsObject() && method.object->IsCallable()) {
        const Result converted =
            call_.interpreter.CallFunction(method, value, {Value::String(key)});
        if (converted.IsAbrupt()) {
          failed_ = converted;
          threw_ = true;
          return false;
        }
        value = converted.value;
      }
    }
    if (replacer_.IsObject()) {
      const Result replaced = call_.interpreter.CallFunction(
          replacer_, holder, {Value::String(key), value});
      if (replaced.IsAbrupt()) {
        failed_ = replaced;
        threw_ = true;
        return false;
      }
      value = replaced.value;
    }
    return true;
  }

  void Newline(int depth, std::string& out) const {
    if (indent_.empty()) {
      return;
    }
    out.push_back('\n');
    for (int i = 0; i < depth; ++i) {
      out += indent_;
    }
  }

  bool WriteArray(const Value& value, int depth, std::string& out) {
    const std::size_t count = value.object->ElementCount();
    if (count == 0) {
      out += "[]";
      return true;
    }
    out.push_back('[');
    for (std::size_t i = 0; i < count; ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      Newline(depth + 1, out);
      // A hole and an unserializable element are both null here: an array's
      // shape has to survive, so nothing may be dropped.
      if (!Write(value, std::to_string(i), value.object->GetElement(i), depth + 1, out)) {
        if (threw_) {
          return false;
        }
        out += "null";
      }
    }
    Newline(depth, out);
    out.push_back(']');
    return true;
  }

  bool WriteObject(const Value& value, int depth, std::string& out) {
    std::string body;
    bool first = true;
    for (const std::string& key : value.object->Keys()) {
      if (filtering_ &&
          std::find(allowed_.begin(), allowed_.end(), key) == allowed_.end()) {
        continue;
      }
      // Through the interpreter's read so a getter runs, which is the whole
      // reason a page defines one on data it means to serialize.
      const Value property = call_.interpreter.GetPropertyValue(value, key);
      std::string written;
      if (!Write(value, key, property, depth + 1, written)) {
        if (threw_) {
          return false;
        }
        continue;  // an undefined property is omitted, not written as null
      }
      if (!first) {
        body.push_back(',');
      }
      first = false;
      Newline(depth + 1, body);
      WriteString(key, body);
      body.push_back(':');
      if (!indent_.empty()) {
        body.push_back(' ');
      }
      body += written;
    }
    if (first) {
      out += "{}";
      return true;
    }
    out.push_back('{');
    out += body;
    Newline(depth, out);
    out.push_back('}');
    return true;
  }

  static void WriteString(const std::string& text, std::string& out) {
    out.push_back('"');
    for (const char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buffer;
          } else {
            out.push_back(c);
          }
          break;
      }
    }
    out.push_back('"');
  }

  NativeCall& call_;
  Value replacer_;
  std::string indent_;
  std::vector<std::string> allowed_;
  std::vector<const Object*> open_;
  Result failed_;
  bool filtering_ = false;
  bool threw_ = false;
};

// The reviver walk: bottom-up, so a reviver sees its children already revived.
// Returns false with the reason on `call` when the reviver threw.
bool Revive(NativeCall& call, const Value& holder, const std::string& key,
            const Value& reviver, int depth, Value& out) {
  if (depth > kMaxJsonDepth) {
    return true;  // the parser already bounded this; nothing here can be deeper
  }
  Value value = call.interpreter.GetPropertyValue(holder, key);
  if (value.IsObject()) {
    if (value.object->GetKind() == Object::Kind::Array) {
      for (std::size_t i = 0; i < value.object->ElementCount(); ++i) {
        Value revived;
        if (!Revive(call, value, std::to_string(i), reviver, depth + 1, revived)) {
          return false;
        }
        // Deleting rather than storing undefined is what the spec says, and a
        // page checking `i in arr` can tell.
        if (revived.IsUndefined()) {
          value.object->Delete(std::to_string(i));
        } else {
          value.object->SetElement(i, revived);
        }
      }
    } else {
      const std::vector<std::string> keys = value.object->Keys();
      for (const std::string& child : keys) {
        Value revived;
        if (!Revive(call, value, child, reviver, depth + 1, revived)) {
          return false;
        }
        if (revived.IsUndefined()) {
          value.object->Delete(child);
        } else {
          value.object->Set(child, revived);
        }
      }
    }
  }
  const Result called =
      call.interpreter.CallFunction(reviver, holder, {Value::String(key), value});
  if (called.IsAbrupt()) {
    call.ThrowValue(called.value);
    return false;
  }
  out = called.value;
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
    const Value reviver = Argument(call.arguments, 1);
    if (!reviver.IsObject() || !reviver.object->IsCallable()) {
      return parsed;
    }
    // The reviver walks a holder object with the result under the empty key,
    // which is the spec's own shape and the reason a top-level reviver call
    // gets `""` as its key.
    const Value root = call.interpreter.NewObjectValue();
    if (!root.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    root.object->Set("", parsed);
    Value revived;
    if (!Revive(call, root, "", reviver, 0, revived)) {
      return Value::Undefined();
    }
    return revived;
  });

  InstallNative(json, "stringify", [](NativeCall& call) {
    // The third argument is an indent: a number is that many spaces, a string
    // is the string itself, and both are capped at ten -- which is the spec's
    // cap and also what keeps a page from asking for a gigabyte of whitespace
    // per nesting level.
    const Value space = Argument(call.arguments, 2);
    std::string indent;
    if (space.IsNumber()) {
      const double count = std::min(10.0, std::max(0.0, std::trunc(space.number)));
      indent.assign(static_cast<std::size_t>(count), ' ');
    } else if (space.IsString()) {
      indent = space.AsString().substr(0, 10);
    }
    JsonWriter writer(call, Argument(call.arguments, 1), std::move(indent));
    // A holder object with the value under the empty key, so the replacer's
    // first call has the shape every later one does.
    const Value root = call.interpreter.NewObjectValue();
    if (!root.IsObject()) {
      return call.Throw("RangeError", "out of memory");
    }
    root.object->Set("", Argument(call.arguments, 0));
    std::string out;
    if (!writer.Write(root, "", Argument(call.arguments, 0), 0, out)) {
      if (!writer.Threw()) {
        return Value::Undefined();
      }
      // A failure raised by the writer is already on the call; one that came
      // out of a `toJSON` or a replacer is carried on the writer.
      return call.HasThrown() ? Value::Undefined()
                              : call.ThrowValue(writer.Failure().value);
    }
    return Value::String(std::move(out));
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
