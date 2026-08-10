#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "js/Heap.h"
#include "js/Value.h"

// WebIDL's argument conversion, in one place.
//
// Every operation on every interface in this module takes its arguments the
// same way, and until this file existed each call site invented that way for
// itself: `js::ToString(Argument(call.arguments, 0))` is the shape almost all
// of them had, and it is wrong in four separate ways at once.
//
//   * It is a *pure* conversion. `js::ToString` cannot call `toString` or
//     `valueOf`, so an object argument became the literal "[object Object]"
//     rather than what the page's own method said -- which is how
//     `new URL(location)` became `https://…/[object%20Object]`.
//   * It cannot fail. A conversion that throws -- a symbol reaching a
//     DOMString, a page's `valueOf` raising -- has to abort the operation, and
//     a `std::string` return has nowhere to put that.
//   * It ignores arity. WebIDL says a call with fewer arguments than the
//     operation requires is a TypeError *before* anything else happens, and
//     `substringData()` returning "" instead of throwing is not a smaller bug
//     than returning the wrong string: it is a page whose error handling never
//     runs.
//   * It ignores the declared *type*. `unsigned long` is not "whatever
//     ToNumber said": it is that number put through ToInteger and then taken
//     modulo 2**32, so `substringData(-1, 0)` is an offset of 4294967295 and
//     an IndexSizeError, not an offset of -1 and a clamp to zero.
//
// So the rule for this module is: an argument is read through exactly one of
// the functions below, and a `false` return means the conversion threw and the
// native must return `call.ThrownValue()` immediately without touching the
// document. That last clause is the one with teeth -- a conversion runs script,
// and script run halfway through a mutation can see a tree no algorithm in the
// specification describes.

namespace microbrowser::bindings {

// Arity. WebIDL: "if the number of arguments is less than the number of
// required arguments, throw a TypeError".
//
// `interface_name` and `operation` are only for the message; nothing tests it,
// but a page that logs the exception is the only reader of a binding's error
// text and "Failed to execute 'substringData' on 'CharacterData'" is what
// every other browser writes.
bool RequireArguments(js::NativeCall& call, std::string_view interface_name,
                      std::string_view operation, std::size_t required);

// DOMString. `undefined` becomes "undefined" and `null` becomes "null", which
// is not a quirk: WebIDL's DOMString conversion is ToString, and the special
// cases live on the *declaration* (`DOMString?`, or an optional argument with
// a default) rather than on the type.
bool ToDomString(js::NativeCall& call, const js::Value& value, std::string& out);

// `DOMString?`. A null or undefined argument is a real null rather than the
// text "null" -- `setAttributeNS(null, …)` is the common spelling of "no
// namespace" and every namespace-aware API takes it.
bool ToNullableDomString(js::NativeCall& call, const js::Value& value, bool& is_null,
                         std::string& out);

// How an out-of-range number reaches an integer type. WebIDL's default is to
// wrap; `[EnforceRange]` and `[Clamp]` are the two extended attributes that
// change it, and which one an operation uses is part of its declaration.
enum class IntegerRange : std::uint8_t { Modulo, Enforce, Clamp };

// `unsigned long`: ToNumber, then the range rule. A non-finite value is a
// TypeError under Enforce and zero under Modulo -- both of which the
// specification spells out rather than leaving to the C++ cast, because
// `static_cast<uint32_t>(1e300)` is undefined behaviour and a page can write
// that literal. This browser's fuzzer found that exact shape once, in `~`.
//
// One width, because one width is what this module's IDL declares today. The
// signed and 64-bit forms are two lines each inside ConvertInteger when
// something needs them; what is deliberately not here is a set of conversions
// with no caller, which is a shape nothing can hold to being right.
bool ToUnsignedLong(js::NativeCall& call, const js::Value& value, IntegerRange range,
                    std::uint32_t& out);

// `boolean` is ToBoolean, which cannot throw and cannot run script. It is here
// so that a call site reads like the IDL it implements rather than reaching
// past this header for the one conversion that happens to be free.
inline bool ToIdlBoolean(const js::Value& value) { return js::ToBoolean(value); }

// The DOM's name productions, which are the other half of "check the argument
// before touching the tree".
//
// A name is not a free-form DOMString: `createElement("fo o")` and
// `setAttribute("a=b", …)` are InvalidCharacterError, and they have to be,
// because an element or attribute whose name contains a space or a `>` is one
// no serialiser can write out and no parser can read back. Getting this wrong
// is not a missing exception -- it is a document that cannot round-trip.
//
// The two rules differ by one character: an attribute name may not contain
// U+003D (=), because that is what separates it from its value in the markup.
enum class NameKind : std::uint8_t { Element, Attribute };
bool IsValidLocalName(std::string_view name, NameKind kind);

// "Validate and extract a namespace and qualifiedName". Splits `qualified` at
// its first colon and checks the four namespace rules the DOM states, throwing
// InvalidCharacterError or NamespaceError as appropriate; false means it threw.
//
// `namespace_uri` is empty-and-null-together: the specification's first step
// turns an empty namespace into a null one, so a single `is_null` flag beside
// the text is the whole of that distinction.
bool ValidateAndExtract(js::NativeCall& call, bool namespace_is_null,
                        std::string_view namespace_uri, const std::string& qualified,
                        NameKind kind, std::string& prefix_out, std::string& local_out);

// The DOM's string offsets are UTF-16 code units, because a DOMString is a
// sequence of them. `data.length`, every CharacterData offset and every Range
// boundary are counted this way, and the storage under them is UTF-8 -- so
// these two wrap `js::Utf16Length` / `js::SubstringUnits` under names that say
// which measurement is meant at the call site.
std::size_t DomStringLength(std::string_view text);
std::string DomSubstring(std::string_view text, std::size_t begin, std::size_t end);

}  // namespace microbrowser::bindings
