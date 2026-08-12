#pragma once

// What "this IDL attribute reflects that content attribute" means, as data.
//
// HTML defines a dozen reflection *kinds* -- a string, a URL, an enumerated
// value limited to known keywords, a boolean, five flavours of integer, two of
// double -- and then applies them a few hundred times across the element
// interfaces. Writing the getter and setter out per attribute is how a browser
// ends up with `td.colSpan` clamping and `col.span` not, from the same
// paragraph of the same specification.
//
// So the kind is a value here and the algorithm exists once, in
// ReflectedAttributes.cpp. This header is the vocabulary; ReflectionTable.cpp
// is the table; that file is the algorithm.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "dom/Node.h"
#include "js/Interpreter.h"

namespace microbrowser::bindings {

class DomBindings;

enum class Reflect : std::uint8_t {
  // A string both ways: the attribute's value, or "" when it is absent.
  Text,
  // The same, except that a JavaScript `null` assigned to it becomes the empty
  // string rather than "null" -- Web IDL's [LegacyNullToEmptyString], which
  // HTML puts on the colour and margin attributes so that `body.bgColor = null`
  // clears the colour instead of asking for one named "null".
  TextNullToEmpty,
  // A URL: absent is "", present is the attribute resolved against the
  // document's address, and an attribute that does not parse is returned as it
  // was written. Setting stores the string verbatim -- the resolution is a
  // property of *reading*, which is why `el.src = 'x'; el.getAttribute('src')`
  // is still `x`.
  Url,
  // A URL whose missing *and empty* value is the document's own address. Two
  // attributes have this shape -- a form's `action` and a submit control's
  // `formaction` -- because a form with no action posts to the page it is on,
  // and a getter answering "" would make that indistinguishable from a form
  // that posts to the root.
  Url_OrDocumentAddress,
  // Limited to known keywords. Getting answers with the canonical spelling of
  // whichever keyword the attribute is an ASCII case-insensitive match for, the
  // invalid value default when it matches none, and the missing value default
  // when the attribute is absent. Setting writes the string through: the
  // canonicalisation is on the *read* side only.
  Enumerated,
  // Presence: `disabled` is true when the attribute is there whatever it says,
  // and setting it false removes the attribute rather than writing "false".
  Boolean,
  // The five integer kinds, which differ only in what they do with a value out
  // of range -- and that difference is the whole of why they are five kinds:
  //
  //   Long                    parse signed; out of range or unparsable -> default
  //   Long_NonNegative        parse non-negative; setting a negative throws
  //   UnsignedLong            parse non-negative; setting > 2^31-1 writes the default
  //   UnsignedLong_NonZero    as above, and setting zero throws
  //   UnsignedLong_Fallback   as above, and setting zero writes the default
  //   UnsignedLong_Clamped    reading clamps into [minimum, maximum]
  Long,
  Long_NonNegative,
  UnsignedLong,
  UnsignedLong_NonZero,
  UnsignedLong_Fallback,
  UnsignedLong_Clamped,
  // A floating-point number, and one limited to values greater than zero --
  // where a set that is not greater than zero is *ignored*, leaving the
  // attribute as it was. That is not the same as writing the default, and
  // `<progress max>` is where the difference shows.
  Double,
  Double_Positive,
  // The one that is not a content attribute at all: a textarea's value is its
  // text. Read as the `value` attribute when something set one and as the
  // element's text otherwise, which is exactly what the engine's own
  // ControlValue does -- the two have to agree or a page reads back something
  // different from what it submits.
  TextareaValue,
  // `nonce`, which reflects in one direction only.
  //
  // The content attribute feeds the value into an internal slot, and the IDL
  // attribute reads and writes *that* -- so assigning `el.nonce` leaves the
  // content attribute alone. It reads as a bug and it is the point: a
  // stylesheet may say `script[nonce=a] { background: url(...a) }`, so an
  // attribute a page can read back through the DOM is a nonce an injected
  // stylesheet can exfiltrate one character at a time. Hiding it in a slot is
  // what CSP's nonce-hiding requires, and the reflection suite asserts the
  // attribute does not move.
  Nonce,
};

// One reflected IDL attribute.
//
// The fields after `kind` are the per-kind parameters, and every one of them is
// a number or a name HTML states outright for that attribute -- `<input size>`
// defaults to 20, `<td rowspan>` clamps to 65534. Leaving them at zero is how
// an attribute HTML gives no default gets the kind's own.
struct Reflection {
  const char* interface;
  const char* property;
  const char* attribute;
  Reflect kind;

  // Enumerated: the keywords, in their canonical spelling.
  std::span<const std::string_view> keywords{};
  // Enumerated: the missing value default, and the invalid value default when
  // it differs. `nullptr` means the IDL attribute answers `null` -- which is a
  // state only a nullable attribute has, and the reason `img.crossOrigin`
  // unset is not the same as `img.crossOrigin = ''`.
  const char* missing = "";
  const char* invalid = nullptr;
  bool nullable = false;

  // Numeric: the default value, and the range a clamped attribute reads into.
  double fallback = 0;
  double minimum = 0;
  double maximum = 0;
};

// The handful of reflections that live on `Document` and describe an element
// somewhere else: `document.dir` is the document element's, and the five
// presentational colours are the body's. They are the same algorithms over a
// different target, which is why they are the same struct with a redirection
// beside it rather than six hand-written accessors.
struct DocumentReflection {
  Reflection reflection;  // `interface` is unused: these go on Document.
  // Which element answers. The body, or the document element.
  bool on_body;
};

std::span<const Reflection> ReflectionTable();
std::span<const DocumentReflection> DocumentReflectionTable();

// Performing a reflection: HTML's dozen algorithms, over the two tables above.
//
// A type of its own rather than another handful of `DomBindings` methods, which
// is the thing that module's line cap keeps asking for. It holds nothing -- it
// is a reference to the binding layer with the reflection algorithms attached --
// so it is constructed at the point of use and the accessors it installs
// capture the `DomBindings*`, not a `Reflector`.
//
// It borrows two things and only two. Attribute *writes* go through
// `DomBindings::SetElementAttribute`, so a reflected write runs the same
// custom-element reaction and records the same mutation a `setAttribute` does --
// two paths there is how an observer ends up seeing half a page's changes. And
// a URL attribute resolves against the document's own address, which lives
// there too.
class Reflector {
 public:
  explicit Reflector(DomBindings& bindings) : bindings_(bindings) {}

  // Every accessor in both tables, onto the interface prototype each belongs
  // to. Called once, after every interface exists.
  void Install();

 private:
  js::Value Get(const Reflection& entry, dom::Element& element, js::Object* wrapper);
  js::Value Set(const Reflection& entry, dom::Element& element, js::NativeCall& call,
                js::Object* wrapper);
  // A URL attribute as the page would follow it. Answers with the input when it
  // does not parse, which is what HTML says a reflected URL returns.
  std::string Resolve(std::string_view relative) const;
  void InstallDocumentReflections();
  void InstallHyperlinkElementUtils();

  DomBindings& bindings_;
};

}  // namespace microbrowser::bindings
