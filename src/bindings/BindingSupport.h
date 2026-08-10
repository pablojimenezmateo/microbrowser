#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/Geometry.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/StringUtil.h"

// Shared by the binding translation units, and private to the module: a
// binding is an implementation detail of the seam, not part of its interface,
// so this header is deliberately absent from MODULE.deps' `public:` list.

namespace microbrowser::bindings {

// Where a wrapper keeps the node it stands for, and where a native keeps the
// bindings instance it belongs to. `#` names, the same convention private
// class fields and the engine's other internal slots already use.
inline constexpr const char* kNodeSlot = "#node";
inline constexpr const char* kOwnerSlot = "#bindings";
inline constexpr const char* kBlobBodySlot = "#blobBody";
inline constexpr const char* kBlobTypeSlot = "#blobType";
inline constexpr const char* kBlobMarkerSlot = "#isBlob";
// Where the document wrapper keeps its `readyState`. A hidden property rather
// than a C++ member, because the collector can see a property and cannot see a
// `js::Value` in a field -- the same rule the wrapper cache follows.
inline constexpr const char* kReadyStateSlot = "#readyState";
inline constexpr const char* kCSSStyleSheetMarkerSlot = "#isCSSStyleSheet";
// Where an element's wrapper keeps its one `DOMTokenList`. The list is live,
// so this is identity rather than a cache: `el.classList === el.classList` is
// something pages assert, and a fresh object per read answers false.
inline constexpr const char* kClassListSlot = "#classList";
// A heap-allocated `std::shared_ptr<std::string>` holding the parsed text.
// Shared by every root that adopts the sheet; `replaceSync` mutates it in place.
inline constexpr const char* kCSSSheetStorageSlot = "#cssSheetStorage";

// A C++ pointer, as a value script can hold but not usefully forge. It travels
// as a double, which holds a 53-bit integer exactly -- more than any address on
// a machine with canonical-form pointers.
inline js::Value PointerValue(const void* pointer) {
  return js::Value::Number(static_cast<double>(reinterpret_cast<std::uintptr_t>(pointer)));
}

// An argument, or undefined when the caller passed fewer.
//
// A local copy rather than `js::Argument`, which lives in a header `src/js`
// keeps to itself. That is the module boundary working: a binding is a
// consumer of the engine's *public* surface, and reaching past it for a
// three-line helper would be the first crack in the line this module's
// manifest calls a security boundary.
inline js::Value Argument(const std::vector<js::Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : js::Value::Undefined();
}

// Convert a binding argument the way script operators do: `@@toPrimitive` /
// `toString` / `valueOf`. `js::ToString` invents "[object Object]" for ordinary
// objects and cannot see `Location`/`URL.prototype.toString` — which is how
// `new URL(location)` became `https://…/[object%20Object]` and youtube's
// consent `continue=` URL broke.
inline bool CoerceToString(js::NativeCall& call, const js::Value& value, std::string& out) {
  const js::Result converted = call.interpreter.ToStringOf(value, out);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  return true;
}

// `DOMException`: the type a *web API* throws, as against the `Error` types the
// language throws. Defined in DomExceptions.cpp, where the WebIDL error-names
// table lives.
//
// `ThrowDom` is the only way a binding should raise one, and it is a free
// function rather than a method on anything because the call sites are natives
// with no receiver in common. `name` is a WebIDL error name -- "NotFoundError",
// "InvalidStateError" -- and the numeric `code` a page reads is derived from it
// rather than passed, so the two cannot disagree.
void InstallDomException(js::Interpreter& interpreter);
js::Value MakeDomException(js::Interpreter& interpreter, std::string_view name,
                           std::string message);
js::Value ThrowDom(js::NativeCall& call, std::string_view name, std::string message);

class DomBindings;

// What a `Proxy` wraps, following a chain of them, or the object itself.
//
// A live binding object -- `el.style`, `el.dataset`, `el.classList` -- is a
// Proxy over a plain target that carries the element. When such an object is
// the *receiver* of a method read off a shared prototype, `this` is the proxy
// and the element is on the target behind it, so every "which node is this?"
// question has to look through. Bounded, because a page can nest proxies.
inline js::Object* BehindProxies(js::Object* object) {
  for (int depth = 0; object != nullptr && depth < 16; ++depth) {
    if (object->GetKind() != js::Object::Kind::Proxy) {
      return object;
    }
    const js::Value* behind = object->GetOwn("#target");
    object = behind != nullptr && behind->IsObject() ? behind->object : nullptr;
  }
  return object;
}

// The node behind a wrapper, or null for anything that is not one.
//
// Every binding starts here rather than trusting its receiver, because a page
// can call one on anything: `Element.prototype.appendChild.call(7, x)` is legal
// JavaScript and must be a TypeError rather than a jump through a bad pointer.
inline dom::Node* NodeOf(const js::Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  const js::Object* behind = BehindProxies(value.object);
  const js::Value* slot = behind == nullptr ? nullptr : behind->GetOwn(kNodeSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(slot->number));
}

// A property key as an array index, or `kNotAnIndex`.
//
// The canonical form only: "0", "1", … and nothing with a leading zero, a
// sign or a decimal point. That strictness is the specification's -- `list["01"]`
// is a named property and not element 1 -- and it is what keeps an indexed
// getter from shadowing a name a page put on the object.
inline constexpr std::size_t kNotAnIndex = static_cast<std::size_t>(-1);
inline std::size_t ArrayIndexOf(std::string_view text) {
  if (text.empty() || text.size() > 10) {
    return kNotAnIndex;
  }
  if (text.size() > 1 && text[0] == '0') {
    return kNotAnIndex;
  }
  std::size_t index = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return kNotAnIndex;
    }
    index = index * 10 + static_cast<std::size_t>(c - '0');
  }
  return index > 4294967294ULL ? kNotAnIndex : index;
}

// The key a Proxy trap was handed. It arrives as a value -- a string or a
// symbol -- and every trap in this module has to turn it back into the key it
// came from without letting a symbol's description collide with a string.
inline js::PropertyKey KeyOfTrapArgument(const js::Value& value) {
  if (value.IsSymbol()) {
    return js::PropertyKey::Symbol(value.object);
  }
  return js::PropertyKey(js::ToString(value));
}

// The bindings instance a native belongs to. Carried on the function object
// rather than captured, because a capture is invisible to the collector and a
// raw pointer in one is a lifetime nobody is tracking.
inline DomBindings* OwnerOf(const js::NativeCall& call) {
  const js::Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<DomBindings*>(static_cast<std::uintptr_t>(slot->number));
}

// A rectangle as a page reads one: the eight members of a `DOMRect`, where
// `x`/`y`/`width`/`height` and `top`/`right`/`bottom`/`left` are the same four
// numbers under two names.
//
// One implementation rather than one per caller, which is not tidiness: a page
// that gets one set of names and not the other silently computes zero, and
// `getBoundingClientRect`, an intersection record and a resize record all have
// to answer with the same object shape. Defined in GeometryBindings.cpp.
js::Value MakeDomRect(js::Interpreter& interpreter, const GeometryRect& rect);

// The one ASCII lower-caser, in util. This name stays because it is used at
// forty call sites in this module and `util::` on each of them buys nothing.
inline std::string LowerCase(std::string_view text) { return util::AsciiLowerCase(text); }

// What a node calls itself: `nodeName`, and for an element `tagName` too.
//
// **They are the same string, and this exists because they were not.**
// `nodeName` upper-cased the parser's tag name and `tagName` handed back the
// stored lower-case one, with a comment saying the two "deliberately differ".
// The DOM says the opposite -- Element.tagName *is* its qualified name, which
// for an HTML element in an HTML document is upper case -- and every browser
// agrees, so `if (el.tagName === 'SCRIPT')` is written everywhere and was
// silently false here. One function so a third caller cannot invent a third
// answer.
//
// Upper case unconditionally, which is right for this browser rather than a
// simplification: the rule is really "HTML elements in HTML documents", and
// there is one tree here with no XML in it. SVG is rendered from its own
// decoder and never becomes elements.
std::string NodeNameOf(const dom::Node& node);

// A deep or shallow copy of `node`, for `cloneNode` and `document.importNode`.
//
// One implementation rather than two: Polymer stamps a template with
// `document.importNode(template.content, true)`, and until that existed the
// call threw and every custom element's template stayed empty -- which is why
// youtube.com painted a white page with two upgraded hosts and no shadow
// trees. `cloneNode` and `importNode` must describe the same tree.
std::unique_ptr<dom::Node> CloneDomNode(const dom::Node& node, bool deep);

// Polymer / Lit binding tokens left in attribute values until effects replace
// them. Kept visible to getAttribute/clone so annotation parsing can see them;
// attributeChangedCallback must not treat them as serial values (TD-0017).
// Template contents must stay un-upgraded so these tokens survive until
// `_parseTemplate` (see InsertFragmentChildren).
inline bool IsTemplateBindingToken(std::string_view value) {
  if (value.size() < 4) {
    return false;
  }
  if (value[0] == '[' && value[1] == '[' && value[value.size() - 2] == ']' &&
      value[value.size() - 1] == ']') {
    return true;
  }
  if (value[0] == '{' && value[1] == '{' && value[value.size() - 2] == '}' &&
      value[value.size() - 1] == '}') {
    return true;
  }
  return false;
}

// URL parts a page reads off `location`, `URL`, and `HTMLAnchorElement`
// (HTMLHyperlinkElementUtils). One splitter so `a.pathname` cannot disagree
// with `location.pathname` for the same string — youtube's searchbox parses
// `location.href` through a throwaway `<a>` (`n0n` / `w1v.pathname`) and
// threw when pathname was missing (TD-0026).
struct HrefParts {
  std::string protocol;  // "https:"
  std::string host;      // "example.org:8080"
  std::string hostname;  // "example.org"
  std::string port;
  std::string pathname;  // "/a/b"
  std::string search;    // "?q=1"
  std::string hash;      // "#top"
  std::string origin;    // "https://example.org:8080", or "null"
};

HrefParts SplitHref(std::string_view href);

}  // namespace microbrowser::bindings
