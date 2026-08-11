#include "bindings/WebIdl.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "bindings/BindingSupport.h"
#include "js/Interpreter.h"
#include "js/StringUnits.h"
#include "util/StringUtil.h"

// WebIDL §3.2, the ECMAScript-to-IDL conversions, for the types this module's
// interfaces actually declare. The reasoning for the file is in the header; what
// is here is the arithmetic, and the arithmetic is the part that is easy to get
// subtly wrong.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// ToIntegerOrInfinity, spelled out: truncate toward zero, NaN is +0, and the
// infinities survive so the range rules below can see them. `std::trunc` on a
// double never overflows, which is exactly why the truncation happens here in
// floating point rather than in the cast.
double IntegerPart(double number) {
  if (std::isnan(number)) {
    return 0.0;
  }
  if (std::isinf(number)) {
    return number;
  }
  return std::trunc(number);
}

// The shared body of every integer conversion: run the value through the
// interpreter's ToNumber -- which can call a page's `valueOf` and therefore can
// throw -- and then apply the range rule against [lower, upper].
//
// `wrapped` is where Modulo puts its answer: the value taken modulo
// 2**bits and folded back into the type's range. It is computed in `double`
// via std::fmod because the input is a double that may be far outside any
// integer type, and a cast of one of those is undefined behaviour rather than
// a wrap. That is not theoretical -- `substringData(1e300, 0)` is a line a page
// can write, and this browser's fuzzer found the same shape in `~`.
bool ConvertInteger(NativeCall& call, const Value& value, IntegerRange range, double lower,
                    double upper, double modulus, double& out) {
  double number = 0.0;
  const js::Result converted = call.interpreter.ToNumberOf(value, number);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  switch (range) {
    case IntegerRange::Enforce:
      // [EnforceRange]: NaN and the infinities are a TypeError, and so is
      // anything outside the type -- checked *before* truncation, because the
      // specification truncates first and then compares, and the two differ
      // only for values that are out of range either way.
      if (std::isnan(number) || std::isinf(number)) {
        (void)call.Throw("TypeError", "value is not a finite number");
        return false;
      }
      number = IntegerPart(number);
      if (number < lower || number > upper) {
        (void)call.Throw("TypeError", "value is out of range");
        return false;
      }
      out = number;
      return true;
    case IntegerRange::Clamp:
      // [Clamp]: NaN is zero, and the ends of the range are saturating. The
      // rounding is to-nearest-even rather than truncating, which is the one
      // place WebIDL does not use ToInteger.
      if (std::isnan(number)) {
        out = 0.0;
        return true;
      }
      number = std::min(std::max(number, lower), upper);
      out = std::nearbyint(number);
      return true;
    case IntegerRange::Modulo:
      break;
  }
  number = IntegerPart(number);
  if (std::isinf(number)) {
    out = 0.0;  // ToUint32/ToInt32 of an infinity is +0.
    return true;
  }
  double wrapped = std::fmod(number, modulus);
  if (wrapped < 0.0) {
    wrapped += modulus;
  }
  // `wrapped` is now in [0, modulus). Signed types fold the upper half down.
  if (lower < 0.0 && wrapped > upper) {
    wrapped -= modulus;
  }
  out = wrapped;
  return true;
}

}  // namespace

bool RequireArguments(NativeCall& call, std::string_view interface_name,
                      std::string_view operation, std::size_t required) {
  if (call.arguments.size() >= required) {
    return true;
  }
  std::string message = "Failed to execute '";
  message.append(operation);
  message.append("' on '");
  message.append(interface_name);
  message.append("': ");
  message.append(std::to_string(required));
  message.append(required == 1 ? " argument required, but only " : " arguments required, but only ");
  message.append(std::to_string(call.arguments.size()));
  message.append(" present.");
  (void)call.Throw("TypeError", std::move(message));
  return false;
}

bool ToDomString(NativeCall& call, const Value& value, std::string& out) {
  const js::Result converted = call.interpreter.ToStringOf(value, out);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  return true;
}

bool ToNullableDomString(NativeCall& call, const Value& value, bool& is_null, std::string& out) {
  // WebIDL: only `null` is the null; `undefined` converts like any other value,
  // *unless* the argument is optional and undefined means "not passed" -- which
  // is a decision the caller makes by not calling this at all. Both are treated
  // as null here because every nullable DOMString this module takes is also
  // optional in practice (`setAttributeNS(undefined, …)` means no namespace to
  // every page that writes it, and to every other browser).
  if (value.IsNullish()) {
    is_null = true;
    out.clear();
    return true;
  }
  is_null = false;
  return ToDomString(call, value, out);
}

bool ToUnsignedLong(NativeCall& call, const Value& value, IntegerRange range,
                    std::uint32_t& out) {
  double converted = 0.0;
  if (!ConvertInteger(call, value, range, 0.0, 4294967295.0, 4294967296.0, converted)) {
    return false;
  }
  out = static_cast<std::uint32_t>(converted);
  return true;
}

bool IsValidDoctypeName(std::string_view name) {
  for (const char c : name) {
    if (c == '\0' || c == '>' || util::IsHtmlWhitespace(c)) {
      return false;
    }
  }
  return true;
}

bool IsValidLocalName(std::string_view name, NameKind kind) {
  // A byte-wise walk over what the specification states in code points, and
  // the two agree exactly: every rule here is either about an ASCII character
  // or about "in the range U+0080 to U+10FFFF", and in UTF-8 the second is
  // precisely "this byte has its high bit set". No decoding, and no way for a
  // truncated sequence to slip past a code-point check.
  if (name.empty()) {
    return false;
  }
  const auto high = [](char c) { return (static_cast<unsigned char>(c) & 0x80U) != 0; };
  const auto forbidden_anywhere = [kind](char c) {
    return c == '\0' || util::IsHtmlWhitespace(c) || c == '/' || c == '>' ||
           (kind == NameKind::Attribute && c == '=');
  };
  // **An attribute name is only checked for the characters that would break
  // the markup**, and never for how it starts. That is not a shortcut: an
  // element name is a tag, and a tag that begins with a digit or a `^` cannot
  // be written and read back -- but an attribute name is only ever read in the
  // context of a start tag, so `0`, `~`, `'`, `"` and `invalid^Name` all round
  // trip and every browser accepts them. web-platform-tests asserts exactly
  // that (dom/nodes/productions.js lists all five as *valid*), and the cost of
  // getting it the other way round is a page whose `setAttribute` throws where
  // no other browser's does.
  if (kind == NameKind::Attribute) {
    for (const char c : name) {
      if (!high(c) && forbidden_anywhere(c)) {
        return false;
      }
    }
    return true;
  }
  if (util::IsAsciiAlpha(name[0])) {
    for (const char c : name) {
      if (!high(c) && forbidden_anywhere(c)) {
        return false;
      }
    }
    return true;
  }
  if (!high(name[0]) && name[0] != ':' && name[0] != '_') {
    return false;
  }
  for (const char c : name.substr(1)) {
    if (high(c) || util::IsAsciiAlphanumeric(c) || c == '-' || c == '.' || c == ':' || c == '_') {
      continue;
    }
    return false;
  }
  return true;
}

bool ValidateAndExtract(NativeCall& call, bool namespace_is_null, std::string_view namespace_uri,
                        const std::string& qualified, NameKind kind, std::string& prefix_out,
                        std::string& local_out) {
  static constexpr std::string_view kXml = "http://www.w3.org/XML/1998/namespace";
  static constexpr std::string_view kXmlns = "http://www.w3.org/2000/xmlns/";

  prefix_out.clear();
  local_out = qualified;
  bool prefixed = false;
  if (const std::size_t colon = qualified.find(':'); colon != std::string::npos) {
    prefixed = true;
    prefix_out = qualified.substr(0, colon);
    // The *first* colon, so `f:o:o` is the prefix `f` and the local name
    // `o:o` -- which is then a perfectly valid local name, and the refusal it
    // earns is a NamespaceError rather than an InvalidCharacterError.
    local_out = qualified.substr(colon + 1);
  }
  if (prefixed && (prefix_out.empty() || local_out.empty())) {
    (void)ThrowDom(call, "InvalidCharacterError",
                   "'" + qualified + "' is not a valid qualified name");
    return false;
  }
  if (!IsValidLocalName(local_out, kind) ||
      (prefixed && !IsValidLocalName(prefix_out, kind))) {
    (void)ThrowDom(call, "InvalidCharacterError",
                   "'" + qualified + "' is not a valid name");
    return false;
  }
  // The four namespace rules, in the specification's order. Each of them is a
  // page trying to name something it has not given a namespace for -- and the
  // last two are about the two namespaces that are reserved by name, which is
  // why they are stated separately rather than folded together.
  const auto refuse = [&call, &qualified]() {
    (void)ThrowDom(call, "NamespaceError",
                   "'" + qualified + "' is not valid in this namespace");
    return false;
  };
  if (prefixed && namespace_is_null) {
    return refuse();
  }
  if (prefix_out == "xml" && namespace_uri != kXml) {
    return refuse();
  }
  if ((qualified == "xmlns" || prefix_out == "xmlns") && namespace_uri != kXmlns) {
    return refuse();
  }
  if (!namespace_is_null && namespace_uri == kXmlns && qualified != "xmlns" &&
      prefix_out != "xmlns") {
    return refuse();
  }
  return true;
}

bool ToQualifiedName(NativeCall& call, const js::Value& namespace_argument,
                     const js::Value& name_argument, NameKind kind, QualifiedName& out) {
  bool namespace_is_null = false;
  std::string namespace_uri;
  std::string qualified;
  if (!ToNullableDomString(call, namespace_argument, namespace_is_null, namespace_uri) ||
      !ToDomString(call, name_argument, qualified)) {
    return false;
  }
  if (namespace_uri.empty()) {
    namespace_is_null = true;  // step 1: an empty namespace *is* no namespace
  }
  std::string prefix;
  std::string local;
  if (!ValidateAndExtract(call, namespace_is_null, namespace_uri, qualified, kind, prefix,
                          local)) {
    return false;
  }
  out.name_space = namespace_is_null ? dom::NamespaceRef() : dom::NamespaceRef(namespace_uri);
  out.prefix_length = static_cast<std::uint32_t>(prefix.size());
  out.qualified = std::move(qualified);
  return true;
}

bool ToNamespaceAndLocalName(NativeCall& call, const js::Value& namespace_argument,
                             const js::Value& name_argument, dom::NamespaceRef& name_space,
                             std::string& local_name) {
  bool namespace_is_null = false;
  std::string namespace_uri;
  if (!ToNullableDomString(call, namespace_argument, namespace_is_null, namespace_uri) ||
      !ToDomString(call, name_argument, local_name)) {
    return false;
  }
  name_space = namespace_is_null ? dom::NamespaceRef() : dom::NamespaceRef(namespace_uri);
  return true;
}

std::size_t DomStringLength(std::string_view text) { return js::Utf16Length(text); }

std::string DomSubstring(std::string_view text, std::size_t begin, std::size_t end) {
  return js::SubstringUnits(text, begin, end);
}

}  // namespace microbrowser::bindings
