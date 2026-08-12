// The IDL attributes that reflect content attributes.
//
// `el.value = 'x'` and `el.setAttribute('value', 'x')` are the same act, and a
// browser where they are not is one where half a page's writes land somewhere
// nothing reads. Before this file every one of these was a plain property on
// the wrapper object: the assignment succeeded, the value read back, and the
// element it was supposed to describe never changed -- so a form submitted
// without it, the cascade never saw the class, and nothing anywhere reported a
// problem.
//
// It is the shape of the failure that makes it worth its own file rather than
// another handful of accessors: the survey's reddit challenge is
//
//     Object.assign(document.createElement("input"), {name: n, type: "hidden", value: e})
//
// which is three reflected attributes in one expression and no `setAttribute`
// anywhere. Getting `Object.assign` to reach a DOM element is not a special
// case -- it is [[Set]] finding a setter on the prototype chain, which is
// exactly what these are.
//
// **Which attributes** is ReflectionTable.cpp; **how each kind behaves** is
// here. The split is the point: HTML states the twelve reflection algorithms
// once and then applies them a few hundred times, and a browser that writes
// them out per attribute gets `td.colSpan` clamping while `col.span` does not.
//
// The parsers below are transcriptions of HTML's "rules for parsing integers",
// "…non-negative integers" and "…floating-point number values". They are here
// rather than in `util` because they are reflection's, not the language's:
// `util::ParseInt` rejects trailing garbage and these must stop at it, and
// "1. 1" is the number 1 to HTML and an error to everyone else.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/Canvas.h"
#include "bindings/DomBindings.h"
#include "bindings/Network.h"
#include "bindings/Reflection.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

constexpr double kMaxLong = 2147483647.0;
constexpr double kMinLong = -2147483648.0;
constexpr double kTwoToThe32 = 4294967296.0;

// The five characters HTML calls ASCII whitespace. Deliberately not
// `std::isspace`: a vertical tab is whitespace to C and is not to HTML, and
// `"7"` parsing as 7 rather than as an error is a difference the suite
// tests thirty-odd times per numeric attribute.
constexpr bool IsHtmlSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

constexpr bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// "Rules for parsing integers". Returns nothing when the string does not begin
// with an optionally-signed run of digits; stops at the first character that is
// not one, rather than rejecting what follows.
//
// The accumulator is a double because the caller's question is always "is this
// in range", and a value large enough to overflow an integer is already out of
// every range below. It saturates rather than wrapping for the same reason:
// wrapping would put a huge attribute *inside* the range it is far outside of.
std::optional<double> ParseInteger(std::string_view text) {
  std::size_t position = 0;
  double sign = 1;
  while (position < text.size() && IsHtmlSpace(text[position])) {
    ++position;
  }
  if (position >= text.size()) {
    return std::nullopt;
  }
  if (text[position] == '-') {
    sign = -1;
    ++position;
  } else if (text[position] == '+') {
    ++position;
  }
  if (position >= text.size() || !IsAsciiDigit(text[position])) {
    return std::nullopt;
  }
  double value = 0;
  while (position < text.size() && IsAsciiDigit(text[position])) {
    if (value < 1e18) {
      value = value * 10 + static_cast<double>(text[position] - '0');
    }
    ++position;
  }
  // Zero has no sign here, which is why "-0" is a *non-negative* integer to
  // HTML and `img.hspace = '-0'` reads back as 0 rather than falling back.
  return value == 0 ? 0.0 : sign * value;
}

// "Rules for parsing non-negative integers": the above, with a negative result
// treated as an error.
std::optional<double> ParseNonNegativeInteger(std::string_view text) {
  const std::optional<double> value = ParseInteger(text);
  if (!value.has_value() || *value < 0) {
    return std::nullopt;
  }
  return value;
}

// "Rules for parsing floating-point number values". Transcribed step for step,
// including the two things that surprise: it stops at the first character it
// cannot use (so "1. 1" is 1), and a value that overflows to infinity is an
// error rather than an infinity.
std::optional<double> ParseFloatingPoint(std::string_view input) {
  std::size_t position = 0;
  double value = 1;
  double divisor = 1;
  double exponent = 1;
  while (position < input.size() && IsHtmlSpace(input[position])) {
    ++position;
  }
  if (position >= input.size()) {
    return std::nullopt;
  }
  if (input[position] == '-') {
    value = -1;
    divisor = -1;
    ++position;
  } else if (input[position] == '+') {
    ++position;
  }
  if (position >= input.size()) {
    return std::nullopt;
  }
  if (input[position] == '.' && position + 1 < input.size() && IsAsciiDigit(input[position + 1])) {
    value = 0;
  } else if (!IsAsciiDigit(input[position])) {
    return std::nullopt;
  } else {
    double whole = 0;
    while (position < input.size() && IsAsciiDigit(input[position])) {
      whole = whole * 10 + static_cast<double>(input[position] - '0');
      ++position;
    }
    value *= whole;
  }
  if (position < input.size() && input[position] == '.') {
    ++position;
    while (position < input.size() && IsAsciiDigit(input[position])) {
      divisor *= 10;
      value += static_cast<double>(input[position] - '0') / divisor;
      ++position;
    }
  }
  if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
    ++position;
    if (position < input.size()) {
      if (input[position] == '-') {
        exponent = -1;
        ++position;
      } else if (input[position] == '+') {
        ++position;
      }
      if (position < input.size() && IsAsciiDigit(input[position])) {
        double magnitude = 0;
        do {
          magnitude = magnitude * 10 + static_cast<double>(input[position] - '0');
          ++position;
        } while (position < input.size() && IsAsciiDigit(input[position]));
        exponent *= magnitude;
        value *= std::pow(10.0, exponent);
      }
    }
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value == 0 ? 0.0 : value;
}

// Web IDL's integer conversion: truncate, then wrap modulo 2^32 into the signed
// or unsigned range. Not a clamp -- a page assigning 2^32 to a `long` gets 0,
// and the reflected setter then writes that.
double ToWebIdlInteger(double number, bool is_signed) {
  if (!std::isfinite(number) || number == 0) {
    return 0;
  }
  double value = std::fmod(std::trunc(number), kTwoToThe32);
  if (value < 0) {
    value += kTwoToThe32;
  }
  if (is_signed && value >= 2147483648.0) {
    value -= kTwoToThe32;
  }
  return value;
}

dom::Element* ElementOf(const js::Value& value) {
  dom::Node* node = NodeOf(value);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

// The invalid value default, which is the missing value default unless the
// attribute names its own. `nullptr` is the null state a nullable attribute has.
const char* InvalidDefault(const Reflection& entry) {
  return entry.invalid != nullptr ? entry.invalid : entry.missing;
}

Value KeywordValue(const char* keyword) {
  return keyword == nullptr ? Value::Null() : Value::String(std::string(keyword));
}

// ASCII case folding, and deliberately not Unicode's. A KELVIN SIGN folds to
// `k` under full case folding, so `type="checKbox"` would be a checkbox in
// a browser that used it -- a keyword matched by a string no author could have
// written, which is a parser difference a page gets to choose between.
bool AsciiEqualsIgnoreCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

// The keyword the content attribute puts the element in the state of, in its
// canonical spelling -- so `dir="LTR"` reads back as "ltr" while the attribute
// itself keeps what was written.
Value EnumeratedValue(const Reflection& entry, const std::string* attribute) {
  if (attribute == nullptr) {
    return KeywordValue(entry.missing);
  }
  for (const std::string_view keyword : entry.keywords) {
    if (AsciiEqualsIgnoreCase(*attribute, keyword)) {
      return Value::String(std::string(keyword));
    }
  }
  return KeywordValue(InvalidDefault(entry));
}

// A number as a content attribute: "the shortest string representing the number
// as a valid integer", which is what JavaScript's own number-to-string produces
// for an integral double -- including "0" for negative zero.
std::string NumberAttribute(double value) { return js::NumberToString(value); }

// Where a nonce lives once it is not in the attribute: a hidden slot on the
// element's wrapper, which is the closest this browser has to Web IDL's
// [[CryptographicNonce]].
constexpr const char* kNonceSlot = "#cryptographicNonce";

// Whether the element is in a document, which is the one condition under which
// HTML lets the nonce setter write the content attribute back. A detached
// element keeps its nonce hidden, which is what the reflection suite asserts
// and what stops `document.createElement('script').nonce` from being a way to
// read one back out of the DOM.
// `<canvas>`'s two reflected dimensions, which are also the backing store's
// size -- so writing either has to reach the surface as well as the attribute.
//
// Both spellings converge here rather than only the IDL one, and that is the
// point: `canvas.setAttribute('width', '50')` resizes the canvas in every
// browser, and an accessor pair that owned the resize would have missed it.
constexpr int kDefaultCanvasWidth = 300;
constexpr int kDefaultCanvasHeight = 150;

bool IsCanvas(const dom::Element& element) {
  return element.Namespace().IsHtml() && element.TagName() == "canvas";
}

int CanvasDimension(const dom::Element& element, const char* attribute, int fallback) {
  const std::string* value = element.GetAttribute(attribute);
  const std::optional<double> parsed =
      value == nullptr ? std::nullopt : ParseNonNegativeInteger(*value);
  if (!parsed.has_value() || *parsed > kMaxLong) {
    return fallback;
  }
  return static_cast<int>(*parsed);
}

// Called from both attribute writes, including the removal -- taking the
// surface rather than the binding layer so that it stays a free function and
// `DomBindings` grows no declaration for a `<canvas>` special case.
void ResizeCanvas(CanvasSurface* canvas, dom::Element& element, const std::string& name) {
  if (canvas == nullptr || (name != "width" && name != "height") || !IsCanvas(element)) {
    return;
  }
  canvas->SetCanvasSize(element, CanvasDimension(element, "width", kDefaultCanvasWidth),
                        CanvasDimension(element, "height", kDefaultCanvasHeight));
}

bool IsConnected(const dom::Node& node) {
  const dom::Node* walk = &node;
  while (walk->Parent() != nullptr) {
    walk = walk->Parent();
  }
  return walk->GetKind() == dom::Node::Kind::Document;
}

}  // namespace

void DomBindings::SetElementAttribute(dom::Element& element, const std::string& name,
                                      const std::string& value) {
  // The old value is read before the write, because that is what a reaction is
  // given and there is no second chance to ask.
  const std::string* previous = element.GetAttribute(name);
  const Value old_value = previous == nullptr ? Value::Null() : Value::String(*previous);
  element.SetAttribute(name, value);
  AfterAttributeWrite(element, name, old_value, Value::String(value));
}

void DomBindings::SetElementAttributeNS(dom::Element& element, dom::NamespaceRef name_space,
                                        const std::string& qualified_name,
                                        std::uint32_t prefix_length, const std::string& value) {
  const std::string_view local =
      std::string_view(qualified_name)
          .substr(prefix_length == 0 || prefix_length >= qualified_name.size()
                      ? 0
                      : prefix_length + 1);
  const dom::Attribute* previous = element.GetAttributeNS(name_space, local);
  const Value old_value = previous == nullptr ? Value::Null() : Value::String(previous->value);
  const std::string local_name(local);
  const std::string uri(name_space.Uri());
  element.SetAttributeNS(std::move(name_space), qualified_name, prefix_length, value);
  // The reaction and the record are told the *local* name, which is what the
  // specification hands `attributeChangedCallback` beside the namespace.
  AfterAttributeWrite(element, local_name, old_value, Value::String(value), uri);
}

void DomBindings::RemoveElementAttributeNS(dom::Element& element,
                                           const dom::NamespaceRef& name_space,
                                           std::string_view local_name) {
  const dom::Attribute* previous = element.GetAttributeNS(name_space, local_name);
  if (previous == nullptr) {
    return;
  }
  const Value old_value = Value::String(previous->value);
  const std::string name(local_name);
  const std::string uri(name_space.Uri());
  element.RemoveAttributeNS(name_space, local_name);
  RunAttributeReaction(element, name, old_value, Value::Null());
  RecordMutation(element, "attributes", name, old_value, {}, {}, uri);
}

void DomBindings::AfterAttributeWrite(dom::Element& element, const std::string& name,
                                      const js::Value& old_value, const js::Value& new_value,
                                      std::string_view attribute_namespace) {
  const std::string value = new_value.IsString() ? *new_value.string : std::string();
  // **A media element's `src` set to an object URL is an *attach*, not a fetch.** This is the one path
  // by which a `MediaSource` reaches an element -- `video.src = URL.createObjectURL(source)` -- and it
  // has to be noticed here, at the write, because that is where every spelling of it converges:
  // `video.src =`, `setAttribute('src', …)` and a `srcObject`-style helper all end up on this line.
  // Attaching is also what *opens* the source and queues `sourceopen` (a task —
  // TD-0040), which is how every player learns it may start appending.
  if (media_ != nullptr && name == "src" && value.rfind("blob:", 0) == 0 &&
      media_->IsMedia(element) && media_->AttachMediaSource(element, value)) {
    ScheduleMediaSourceOpened(media_->SourceForObjectUrl(value));
  }
  ResizeCanvas(canvas_, element, name);
  // Keep binding tokens in the attribute map for getAttribute / Polymer
  // annotation parsing, but do not deliver attributeChangedCallback — that
  // path JSON.parses Array/Object types and is what hung youtube (TD-0017).
  if (!IsTemplateBindingToken(value)) {
    RunAttributeReaction(element, name, old_value, new_value);
  }
  RecordMutation(element, "attributes", name, old_value, {}, {}, attribute_namespace);
}

void DomBindings::RemoveElementAttribute(dom::Element& element, const std::string& name) {
  const std::string* previous = element.GetAttribute(name);
  if (previous == nullptr) {
    // "If attr is null, then return" -- removing an attribute that is not
    // there is not a mutation, and an observer told about it counts one record
    // where the specification says none. The NS form already returned here.
    return;
  }
  const Value old_value = Value::String(*previous);
  element.RemoveAttribute(name);
  // Removing `width` is a resize to the default, not "no change": the surface
  // follows the attribute in both directions.
  ResizeCanvas(canvas_, element, name);
  // The reaction is told the new value is null, which is how a class
  // distinguishes "set to empty" from "gone".
  RunAttributeReaction(element, name, old_value, Value::Null());
  RecordMutation(element, "attributes", name, old_value, {}, {});
}

std::string Reflector::Resolve(std::string_view relative) const {
  if (bindings_.network_ == nullptr) {
    return std::string(relative);
  }
  std::string resolved = bindings_.network_->ResolveUrl(relative, bindings_.url_);
  // "If parsing fails, then the value of the content attribute must be
  // returned instead" -- a URL attribute is a string a page wrote, and a
  // browser that answered "" for one it could not parse would lose it.
  return resolved.empty() ? std::string(relative) : resolved;
}

js::Value Reflector::Get(const Reflection& entry, dom::Element& element,
                         js::Object* wrapper) {
  const std::string* attribute = element.GetAttribute(entry.attribute);
  switch (entry.kind) {
    case Reflect::Nonce: {
      // The slot when there is one, and the attribute otherwise -- which is how
      // a nonce written by the parser is readable at all. Nothing writes the
      // slot except this attribute's own setter.
      const Value* stored = wrapper == nullptr ? nullptr : wrapper->GetOwn(kNonceSlot);
      if (stored != nullptr && stored->IsString()) {
        return *stored;
      }
      return Value::String(attribute == nullptr ? std::string() : *attribute);
    }
    case Reflect::Boolean:
      return Value::Bool(attribute != nullptr);
    case Reflect::Enumerated:
      return EnumeratedValue(entry, attribute);
    case Reflect::TextareaValue:
      return Value::String(attribute == nullptr ? element.TextContent() : *attribute);
    case Reflect::Url:
      return Value::String(attribute == nullptr ? std::string()
                                                : Resolve(*attribute));
    case Reflect::Url_OrDocumentAddress:
      // Missing *or empty*: a form with `action=""` submits to the page it is
      // on, and a getter that answered "" would make that indistinguishable
      // from one that submits to the site root.
      return Value::String(attribute == nullptr || attribute->empty()
                               ? bindings_.url_
                               : Resolve(*attribute));
    case Reflect::Long: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseInteger(*attribute);
      const bool usable = parsed.has_value() && *parsed >= kMinLong && *parsed <= kMaxLong;
      return Value::Number(usable ? *parsed : entry.fallback);
    }
    case Reflect::Long_NonNegative:
    case Reflect::UnsignedLong: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseNonNegativeInteger(*attribute);
      const bool usable = parsed.has_value() && *parsed <= kMaxLong;
      return Value::Number(usable ? *parsed : entry.fallback);
    }
    case Reflect::UnsignedLong_NonZero:
    case Reflect::UnsignedLong_Fallback: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseNonNegativeInteger(*attribute);
      const bool usable = parsed.has_value() && *parsed >= 1 && *parsed <= kMaxLong;
      return Value::Number(usable ? *parsed : entry.fallback);
    }
    case Reflect::UnsignedLong_Clamped: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseNonNegativeInteger(*attribute);
      if (!parsed.has_value()) {
        return Value::Number(entry.fallback);
      }
      // Clamped rather than defaulted, which is the difference the name
      // carries: `rowspan="99999"` is 65534 rows, not one.
      return Value::Number(std::min(std::max(*parsed, entry.minimum), entry.maximum));
    }
    case Reflect::Double: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseFloatingPoint(*attribute);
      return Value::Number(parsed.value_or(entry.fallback));
    }
    case Reflect::Double_Positive: {
      const std::optional<double> parsed =
          attribute == nullptr ? std::nullopt : ParseFloatingPoint(*attribute);
      return Value::Number(parsed.has_value() && *parsed > 0 ? *parsed : entry.fallback);
    }
    case Reflect::Text:
    case Reflect::TextNullToEmpty:
      break;
  }
  return Value::String(attribute == nullptr ? std::string() : *attribute);
}

js::Value Reflector::Set(const Reflection& entry, dom::Element& element,
                         js::NativeCall& call, js::Object* wrapper) {
  const Value assigned = Argument(call.arguments, 0);
  switch (entry.kind) {
    case Reflect::Nonce: {
      std::string nonce;
      if (!CoerceToString(call, assigned, nonce)) {
        return call.ThrownValue();
      }
      if (wrapper != nullptr) {
        wrapper->SetHidden(kNonceSlot, Value::String(nonce));
      }
      // Only a connected element's attribute follows. A detached one keeps the
      // value in the slot, so a script that builds an element cannot read the
      // nonce back out of the tree -- see the note on Reflect::Nonce.
      if (IsConnected(element)) {
        bindings_.SetElementAttribute(element, entry.attribute, nonce);
      }
      return Value::Undefined();
    }
    case Reflect::Boolean:
      // Presence, so a false is a removal. Writing "false" into the attribute
      // would leave the element disabled, which is the opposite of what the
      // page asked for and the reason this is not one code path with Text.
      if (js::ToBoolean(assigned)) {
        bindings_.SetElementAttribute(element, entry.attribute, std::string());
      } else {
        bindings_.RemoveElementAttribute(element, entry.attribute);
      }
      return Value::Undefined();
    case Reflect::Enumerated:
      // A nullable enumerated attribute assigned null is the attribute going
      // away -- `img.crossOrigin = null` is how a page opts back out of CORS,
      // and writing the string "null" would opt it into a mode named for one.
      if (entry.nullable && (assigned.IsNull() || assigned.IsUndefined())) {
        bindings_.RemoveElementAttribute(element, entry.attribute);
        return Value::Undefined();
      }
      break;
    case Reflect::TextNullToEmpty:
      if (assigned.IsNull()) {
        bindings_.SetElementAttribute(element, entry.attribute, std::string());
        return Value::Undefined();
      }
      break;
    case Reflect::Long:
    case Reflect::Long_NonNegative:
    case Reflect::UnsignedLong:
    case Reflect::UnsignedLong_NonZero:
    case Reflect::UnsignedLong_Fallback:
    case Reflect::UnsignedLong_Clamped: {
      double number = 0;
      const js::Result converted = call.interpreter.ToNumberOf(assigned, number);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
      const bool is_signed =
          entry.kind == Reflect::Long || entry.kind == Reflect::Long_NonNegative;
      double value = ToWebIdlInteger(number, is_signed);
      if (entry.kind == Reflect::Long_NonNegative && value < 0) {
        return ThrowDom(call, "IndexSizeError",
                        std::string("cannot set ") + entry.property + " to a negative value");
      }
      if (entry.kind == Reflect::UnsignedLong_NonZero && value == 0) {
        return ThrowDom(call, "IndexSizeError",
                        std::string("cannot set ") + entry.property + " to zero");
      }
      // Out of the range the getter can read back is written as the default
      // rather than as itself, so that setting and getting agree.
      const bool below = entry.kind == Reflect::UnsignedLong_Fallback && value < 1;
      if (below || (!is_signed && value > kMaxLong)) {
        value = entry.fallback;
      }
      bindings_.SetElementAttribute(element, entry.attribute, NumberAttribute(value));
      return Value::Undefined();
    }
    case Reflect::Double:
    case Reflect::Double_Positive: {
      double number = 0;
      const js::Result converted = call.interpreter.ToNumberOf(assigned, number);
      if (converted.IsAbrupt()) {
        return call.ThrowValue(converted.value);
      }
      if (!std::isfinite(number)) {
        // Web IDL `double` is the restricted one: an infinity or a NaN is a
        // TypeError rather than an attribute nothing can parse.
        return call.Throw("TypeError",
                          std::string("cannot set ") + entry.property + " to a non-finite number");
      }
      // Positive-only, and a non-positive set is *ignored* -- the attribute
      // keeps what it had. That is not the same as writing the default, which
      // is why `<progress max>` needs its own kind.
      if (entry.kind == Reflect::Double_Positive && !(number > 0)) {
        return Value::Undefined();
      }
      bindings_.SetElementAttribute(element, entry.attribute, NumberAttribute(number));
      return Value::Undefined();
    }
    case Reflect::Text:
    case Reflect::Url:
    case Reflect::Url_OrDocumentAddress:
    case Reflect::TextareaValue:
      break;
  }
  // DOMString conversion runs toString/valueOf (Web IDL). Pure js::ToString
  // invents "[object Object]" for Location/URL — youtube then requested
  // `https://www.youtube.com/[object%20Object]` (seen as consent continue=
  // after redirect). Same class as TD-0027; href on <a> already coerced.
  std::string text;
  if (!CoerceToString(call, assigned, text)) {
    return call.ThrownValue();
  }
  bindings_.SetElementAttribute(element, entry.attribute, text);
  return Value::Undefined();
}

void Reflector::Install() {
  // The accessors capture the *binding layer*, not this object: a Reflector is
  // a reference with algorithms attached, built at the point of use, and one
  // captured here would be a pointer into this frame.
  DomBindings* owner = &bindings_;
  for (const Reflection& entry : ReflectionTable()) {
    const Value* prototype = bindings_.interfaces_.object->GetOwn(entry.interface);
    if (prototype == nullptr || !prototype->IsObject()) {
      continue;
    }
    const Reflection* reflection = &entry;

    const Value get = bindings_.interpreter_->NewNativeValue(entry.property, [reflection,
                                                                             owner](
                                                                                NativeCall& call) {
      dom::Element* element = ElementOf(call.self);
      if (element == nullptr) {
        return Value::Undefined();
      }
      return Reflector(*owner).Get(*reflection, *element,
                                   call.self.IsObject() ? call.self.object : nullptr);
    });
    const Value set = bindings_.interpreter_->NewNativeValue(entry.property, [reflection,
                                                                             owner](
                                                                                NativeCall& call) {
      dom::Element* element = ElementOf(call.self);
      if (element == nullptr) {
        return Value::Undefined();
      }
      return Reflector(*owner).Set(*reflection, *element, call,
                                   call.self.IsObject() ? call.self.object : nullptr);
    });
    if (!get.IsObject() || !set.IsObject()) {
      continue;
    }
    get.object->Set(kOwnerSlot, PointerValue(owner));
    set.object->Set(kOwnerSlot, PointerValue(owner));
    prototype->object->DefineAccessor(entry.property, get.object, set.object);
  }
  InstallDocumentReflections();
  InstallHyperlinkElementUtils();
}

void Reflector::InstallDocumentReflections() {
  const Value* prototype = bindings_.interfaces_.object->GetOwn("Document");
  if (prototype == nullptr || !prototype->IsObject()) {
    return;
  }
  DomBindings* owner = &bindings_;
  for (const DocumentReflection& entry : DocumentReflectionTable()) {
    const DocumentReflection* row = &entry;
    // The element that carries the attribute, found on the *receiver* document
    // rather than the page's -- the same inversion every other `document.*`
    // answer went through when `createHTMLDocument` landed.
    const auto target = [owner, row](NativeCall& call) -> dom::Element* {
      dom::Document* document = owner->DocumentOf(call.self);
      if (document == nullptr) {
        return nullptr;
      }
      return row->on_body ? document->Body() : document->DocumentElement();
    };
    const Value get = bindings_.interpreter_->NewNativeValue(
        entry.reflection.property, [owner, row, target](NativeCall& call) {
          dom::Element* element = target(call);
          // No body yet is not an error: `document.bgColor` before the parser
          // has reached `<body>` is the empty string, not a throw.
          return element == nullptr ? Value::String("")
                                    : Reflector(*owner).Get(row->reflection, *element, nullptr);
        });
    const Value set = bindings_.interpreter_->NewNativeValue(
        entry.reflection.property, [owner, row, target](NativeCall& call) {
          dom::Element* element = target(call);
          if (element == nullptr) {
            return Value::Undefined();
          }
          return Reflector(*owner).Set(row->reflection, *element, call, nullptr);
        });
    if (!get.IsObject() || !set.IsObject()) {
      continue;
    }
    get.object->Set(kOwnerSlot, PointerValue(owner));
    set.object->Set(kOwnerSlot, PointerValue(owner));
    prototype->object->DefineAccessor(entry.reflection.property, get.object, set.object);
  }
}

void Reflector::InstallHyperlinkElementUtils() {
  // HTMLHyperlinkElementUtils on `<a>` and `<area>`. youtube's searchbox
  // resolves `location.href` through `document.createElement('a'); a.href = url;
  // a.pathname` (`n0n`). Without `pathname` that call threw and Enter never
  // navigated (TD-0026).
  //
  // The parts are split from the *resolved* href rather than from the
  // attribute, because that is what they are parts of: `a.href = 'foo'` on a
  // page at `/x/y` has the pathname `/x/foo`, and splitting the attribute would
  // answer `foo` -- which is not a path at all.
  static constexpr const char* kHyperlinkInterfaces[] = {"HTMLAnchorElement", "HTMLAreaElement"};
  DomBindings* owner = &bindings_;
  for (const char* name : kHyperlinkInterfaces) {
    const Value* prototype =
        bindings_.interfaces_.IsObject() ? bindings_.interfaces_.object->GetOwn(name) : nullptr;
    if (prototype == nullptr || !prototype->IsObject()) {
      continue;
    }
    const auto install_part = [owner, prototype](const char* part, auto pick) {
      const Value get =
          owner->interpreter_->NewNativeValue(part, [owner, pick](NativeCall& call) {
            dom::Element* element = ElementOf(call.self);
            const std::string* attribute =
                element == nullptr ? nullptr : element->GetAttribute("href");
            const HrefParts parts = SplitHref(
                attribute == nullptr ? std::string() : Reflector(*owner).Resolve(*attribute));
            return Value::String(pick(parts));
          });
      if (get.IsObject()) {
        get.object->Set(kOwnerSlot, PointerValue(owner));
        prototype->object->DefineAccessor(part, get.object, nullptr);
      }
    };
    install_part("protocol", [](const HrefParts& p) { return p.protocol; });
    install_part("host", [](const HrefParts& p) { return p.host; });
    install_part("hostname", [](const HrefParts& p) { return p.hostname; });
    install_part("port", [](const HrefParts& p) { return p.port; });
    install_part("pathname", [](const HrefParts& p) { return p.pathname; });
    install_part("search", [](const HrefParts& p) { return p.search; });
    install_part("hash", [](const HrefParts& p) { return p.hash; });
    install_part("origin", [](const HrefParts& p) { return p.origin; });
  }
}

}  // namespace microbrowser::bindings
