// `DOMTokenList`: `element.classList`, as the type the DOM defines rather than
// an object with four methods on it.
//
// What was here before was a fresh plain object per read carrying `add`,
// `remove`, `toggle` and `contains`. Every one of those four was wrong in the
// same way -- it treated the attribute as a list of words rather than as an
// *ordered set* -- and the object itself was wrong in three more:
//
//   * `el.classList !== el.classList`. A page that stores the list and compares
//     it later gets false, and `assert_equals(e.classList, expect)` is the
//     first thing web-platform-tests checks.
//   * No `value`, no `toString`, no `item`, no indexed access, no `replace`,
//     no `supports`, no iteration protocol beyond a hand-rolled `Symbol.iterator`
//     -- and no `DOMTokenList` for `instanceof` to answer against.
//   * **No validation.** `classList.add("")` silently did nothing and
//     `classList.add("a b")` wrote a token with a space in it, which is a class
//     attribute no selector can ever match again. The specification throws
//     SyntaxError and InvalidCharacterError respectively, and those two throws
//     are the difference between a page's error handler running and a page
//     quietly styling nothing.
//
// The list is *live*: it holds the element and the attribute name, and reads
// the attribute on every operation. That is why it is a `Proxy` -- indexed
// access has to answer from the attribute as it is now, and own properties
// numbered at construction time would be a snapshot with no hook to refresh it.
// The same shape `el.style` and `el.dataset` already use, for the same reason.
//
// It is parameterised by attribute name rather than hard-wired to `class`,
// because `rel`, `sandbox` and `htmlFor` are the same type over a different
// attribute and a second copy of the ordered-set algorithm is how two of them
// end up disagreeing about what `"a  a"` contains.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"
#include "js/Interpreter.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Which attribute a token list is over. Beside `kNodeSlot` on the same object,
// so the pair travels together and a list cannot be pointed at a second
// attribute after it is made.
constexpr const char* kTokenAttributeSlot = "#tokenAttribute";

// The element and attribute behind `this`, looking through the Proxy.
//
// A method read off `DOMTokenList.prototype` through the get trap is called
// with the *proxy* as its receiver, so this cannot be `NodeOf(call.self)` on
// its own -- `NodeOf` looks through proxies for exactly this reason, and the
// attribute name is read from the same object it found the node on.
struct ListTarget {
  dom::Element* element = nullptr;
  std::string attribute;
};

ListTarget TargetOf(const Value& self) {
  ListTarget found;
  dom::Node* node = NodeOf(self);
  if (node == nullptr || !node->IsElement()) {
    return found;
  }
  const js::Object* behind = BehindProxies(self.object);
  const Value* attribute = behind == nullptr ? nullptr : behind->GetOwn(kTokenAttributeSlot);
  if (attribute == nullptr || !attribute->IsString()) {
    return found;
  }
  found.element = static_cast<dom::Element*>(node);
  found.attribute = attribute->AsString();
  return found;
}

// The attribute's value, or absent.
const std::string* AttributeText(const ListTarget& target) {
  return target.element == nullptr ? nullptr : target.element->GetAttribute(target.attribute);
}

// The ordered set the attribute serialises: split on ASCII whitespace,
// duplicates removed, order of first appearance kept. `"a A B b"` is four
// tokens and `"a b c c b a a b c c"` is three, which is the whole difference
// between a set and a word list.
std::vector<std::string> TokensOf(const ListTarget& target) {
  std::vector<std::string> tokens;
  const std::string* text = AttributeText(target);
  if (text == nullptr) {
    return tokens;
  }
  std::string word;
  const auto flush = [&tokens, &word]() {
    if (word.empty()) {
      return;
    }
    if (std::find(tokens.begin(), tokens.end(), word) == tokens.end()) {
      tokens.push_back(word);
    }
    word.clear();
  };
  for (const char c : *text) {
    if (util::IsHtmlWhitespace(c)) {
      flush();
      continue;
    }
    word.push_back(c);
  }
  flush();
  return tokens;
}

std::string Serialize(const std::vector<std::string>& tokens) {
  std::string out;
  for (const std::string& token : tokens) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += token;
  }
  return out;
}

// A token, validated. Empty is a SyntaxError and any ASCII whitespace in it is
// an InvalidCharacterError -- and both are raised before anything is written,
// because a multi-token `add` that failed halfway would leave the attribute in
// a state no algorithm describes.
//
// The two halves are separate functions because `replace` runs them in a
// different order from everything else: it tests *both* of its arguments for
// emptiness before testing either for whitespace, so `replace(" ", "")` is a
// SyntaxError. `add`/`remove`/`toggle` run both checks per token, in order.
bool ValidateTokenIsNotEmpty(NativeCall& call, const std::string& token) {
  if (!token.empty()) {
    return true;
  }
  (void)ThrowDom(call, "SyntaxError", "the token provided must not be empty");
  return false;
}

bool ValidateTokenHasNoSpace(NativeCall& call, const std::string& token) {
  for (const char c : token) {
    if (util::IsHtmlWhitespace(c)) {
      (void)ThrowDom(call, "InvalidCharacterError",
                     "the token provided ('" + token + "') contains HTML space characters");
      return false;
    }
  }
  return true;
}

bool ValidateToken(NativeCall& call, const std::string& token) {
  return ValidateTokenIsNotEmpty(call, token) && ValidateTokenHasNoSpace(call, token);
}

// Every argument converted and validated first, then applied. Returns false
// when a conversion or a validation threw.
bool CollectTokens(NativeCall& call, std::size_t from, std::vector<std::string>& out) {
  for (std::size_t index = from; index < call.arguments.size(); ++index) {
    std::string token;
    if (!ToDomString(call, call.arguments[index], token)) {
      return false;
    }
    if (!ValidateToken(call, token)) {
      return false;
    }
    out.push_back(std::move(token));
  }
  return true;
}

// Infra's "replace within an ordered set": the first instance of *either*
// token is overwritten with the replacement and every other instance of either
// is dropped. Written out because the obvious reading -- replace each `token`
// with `replacement` -- gives `"a b c".replace("c", "a")` the answer `"a b a"`,
// which is not a set.
void ReplaceInSet(std::vector<std::string>& tokens, const std::string& token,
                  const std::string& replacement) {
  bool placed = false;
  std::vector<std::string> rewritten;
  rewritten.reserve(tokens.size());
  for (const std::string& each : tokens) {
    if (each != token && each != replacement) {
      rewritten.push_back(each);
      continue;
    }
    if (!placed) {
      rewritten.push_back(replacement);
      placed = true;
    }
  }
  tokens = std::move(rewritten);
}

}  // namespace

js::Value DomBindings::TokenListInterface() {
  const Value prototype = MakeInterface("DOMTokenList", Value::Undefined());
  if (!prototype.IsObject()) {
    return prototype;
  }
  if (prototype.object->HasOwn("add")) {
    return prototype;  // already installed
  }
  // `Object.prototype.toString.call(el.classList)` must say `[object
  // DOMTokenList]`. That string is the only way a page can tell this type
  // apart without `instanceof`, and it is what `assert_class_string` -- the
  // check WPT uses on every reflected token list -- reads. Without it the
  // answer was `[object Object]`, which is what a plain object says.
  if (js::Object* tag = interpreter_->SymbolToStringTag()) {
    prototype.object->Set(js::PropertyKey::Symbol(tag), Value::String("DOMTokenList"));
  }

  const auto method = [this, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->SetHidden(name, native);
    }
  };
  const auto accessor = [this, &prototype](const char* name, js::NativeFunction get,
                                           js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  accessor(
      "length",
      [](NativeCall& call) {
        return Value::Number(static_cast<double>(TokensOf(TargetOf(call.self)).size()));
      },
      [](NativeCall&) { return Value::Undefined(); });

  // The stringifier, and it answers with the attribute *as written* rather
  // than with the serialised set: `value` is defined as the attribute's value,
  // so a list over `class="   a  a b"` stringifies to `"   a  a b"` and only a
  // mutation normalises it.
  accessor(
      "value",
      [](NativeCall& call) {
        const ListTarget target = TargetOf(call.self);
        const std::string* text = AttributeText(target);
        return Value::String(text == nullptr ? std::string() : *text);
      },
      [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        const ListTarget target = TargetOf(call.self);
        if (owner == nullptr || target.element == nullptr) {
          return Value::Undefined();
        }
        std::string value;
        if (!ToDomString(call, Argument(call.arguments, 0), value)) {
          return call.ThrownValue();
        }
        owner->SetElementAttribute(*target.element, target.attribute, value);
        return Value::Undefined();
      });

  method("item", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "DOMTokenList", "item", 1)) {
      return call.ThrownValue();
    }
    std::uint32_t index = 0;
    if (!ToUnsignedLong(call, call.arguments[0], IntegerRange::Modulo, index)) {
      return call.ThrownValue();
    }
    const std::vector<std::string> tokens = TokensOf(TargetOf(call.self));
    if (index >= tokens.size()) {
      return Value::Null();
    }
    return Value::String(tokens[index]);
  });

  method("contains", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "DOMTokenList", "contains", 1)) {
      return call.ThrownValue();
    }
    std::string token;
    if (!ToDomString(call, call.arguments[0], token)) {
      return call.ThrownValue();
    }
    // Deliberately *not* validated: `contains` answers a question rather than
    // making a change, so `contains("a b")` is false rather than a throw.
    const std::vector<std::string> tokens = TokensOf(TargetOf(call.self));
    return Value::Bool(std::find(tokens.begin(), tokens.end(), token) != tokens.end());
  });

  method("add", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const ListTarget target = TargetOf(call.self);
    std::vector<std::string> wanted;
    if (!CollectTokens(call, 0, wanted)) {
      return call.ThrownValue();
    }
    if (owner == nullptr || target.element == nullptr) {
      return Value::Undefined();
    }
    std::vector<std::string> tokens = TokensOf(target);
    for (const std::string& token : wanted) {
      if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
        tokens.push_back(token);
      }
    }
    owner->UpdateTokenList(*target.element, target.attribute, tokens);
    return Value::Undefined();
  });

  method("remove", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const ListTarget target = TargetOf(call.self);
    std::vector<std::string> unwanted;
    if (!CollectTokens(call, 0, unwanted)) {
      return call.ThrownValue();
    }
    if (owner == nullptr || target.element == nullptr) {
      return Value::Undefined();
    }
    std::vector<std::string> tokens = TokensOf(target);
    for (const std::string& token : unwanted) {
      tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    }
    owner->UpdateTokenList(*target.element, target.attribute, tokens);
    return Value::Undefined();
  });

  // `toggle(token, force)`. The two forced forms differ from `add`/`remove` in
  // exactly one way and the specification is explicit about it: a forced
  // toggle that changes nothing does **not** run the update steps, so
  // `toggle('a', true)` on `class="a a a  b"` leaves the attribute unnormalised
  // where `add('a')` would rewrite it to `"a b"`.
  method("toggle", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "DOMTokenList", "toggle", 1)) {
      return call.ThrownValue();
    }
    DomBindings* owner = OwnerOf(call);
    const ListTarget target = TargetOf(call.self);
    std::string token;
    if (!ToDomString(call, call.arguments[0], token) || !ValidateToken(call, token)) {
      return call.ThrownValue();
    }
    if (owner == nullptr || target.element == nullptr) {
      return Value::Undefined();
    }
    const bool forced = call.arguments.size() > 1 && !call.arguments[1].IsUndefined();
    const bool force = forced && ToIdlBoolean(call.arguments[1]);
    std::vector<std::string> tokens = TokensOf(target);
    const auto found = std::find(tokens.begin(), tokens.end(), token);
    const bool present = found != tokens.end();
    if (present) {
      if (forced && force) {
        return Value::Bool(true);
      }
      tokens.erase(found);
      owner->UpdateTokenList(*target.element, target.attribute, tokens);
      return Value::Bool(false);
    }
    if (forced && !force) {
      return Value::Bool(false);
    }
    tokens.push_back(token);
    owner->UpdateTokenList(*target.element, target.attribute, tokens);
    return Value::Bool(true);
  });

  method("replace", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "DOMTokenList", "replace", 2)) {
      return call.ThrownValue();
    }
    DomBindings* owner = OwnerOf(call);
    const ListTarget target = TargetOf(call.self);
    std::string token;
    std::string replacement;
    if (!ToDomString(call, call.arguments[0], token) ||
        !ToDomString(call, call.arguments[1], replacement)) {
      return call.ThrownValue();
    }
    // Both validated before either is looked at, so a bad *second* argument
    // throws rather than half-replacing -- and the two *checks* interleave
    // rather than the two tokens: emptiness is tested on both arguments before
    // whitespace is tested on either, which is why `replace(" ", "")` is a
    // SyntaxError and not an InvalidCharacterError.
    if (!ValidateTokenIsNotEmpty(call, token) || !ValidateTokenIsNotEmpty(call, replacement) ||
        !ValidateTokenHasNoSpace(call, token) || !ValidateTokenHasNoSpace(call, replacement)) {
      return call.ThrownValue();
    }
    if (owner == nullptr || target.element == nullptr) {
      return Value::Bool(false);
    }
    std::vector<std::string> tokens = TokensOf(target);
    if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
      return Value::Bool(false);  // and no update steps: nothing changed
    }
    ReplaceInSet(tokens, token, replacement);
    owner->UpdateTokenList(*target.element, target.attribute, tokens);
    return Value::Bool(true);
  });

  // `supports` answers from the attribute's *supported tokens*, and `class`
  // has none -- so this throws for every list this browser makes one of today.
  // A `supports` that answered false would be worse than the throw: it is the
  // feature-detection entry point for `rel` and `sandbox`, and a false there
  // reads as "this browser knows the token and rejects it".
  method("supports", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "DOMTokenList", "supports", 1)) {
      return call.ThrownValue();
    }
    return call.Throw("TypeError", "this attribute has no supported tokens");
  });

  // Iteration. `keys`/`values`/`entries`/`forEach` and `Symbol.iterator` are
  // what an "iterable<DOMString>" declaration means, and a page that reaches
  // for any of them on a list that only had `Symbol.iterator` got undefined.
  const auto snapshot = [](NativeCall& call) {
    std::vector<Value> out;
    for (const std::string& token : TokensOf(TargetOf(call.self))) {
      out.push_back(Value::String(token));
    }
    return out;
  };
  const auto iterator_of = [](NativeCall& call, std::vector<Value> entries) -> Value {
    const Value array = call.interpreter.NewArrayValue(std::move(entries));
    if (!array.IsObject()) {
      return Value::Undefined();
    }
    const Value* protocol =
        array.object->Get(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
    if (protocol == nullptr) {
      return Value::Undefined();
    }
    const js::Result made = call.interpreter.CallFunction(*protocol, array, {});
    return made.IsAbrupt() ? Value::Undefined() : made.value;
  };

  const Value values = interpreter_->NewNativeValue(
      "values", [snapshot, iterator_of](NativeCall& call) { return iterator_of(call, snapshot(call)); });
  if (values.IsObject()) {
    values.object->Set(kOwnerSlot, PointerValue(this));
    prototype.object->SetHidden("values", values);
    prototype.object->SetHidden(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), values);
  }
  method("keys", [snapshot, iterator_of](NativeCall& call) {
    std::vector<Value> keys;
    for (std::size_t index = 0; index < snapshot(call).size(); ++index) {
      keys.push_back(Value::Number(static_cast<double>(index)));
    }
    return iterator_of(call, std::move(keys));
  });
  method("entries", [snapshot, iterator_of](NativeCall& call) {
    std::vector<Value> pairs;
    const std::vector<Value> tokens = snapshot(call);
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      pairs.push_back(call.interpreter.NewArrayValue(
          {Value::Number(static_cast<double>(index)), tokens[index]}));
    }
    return iterator_of(call, std::move(pairs));
  });
  method("forEach", [snapshot](NativeCall& call) -> Value {
    const Value callback = Argument(call.arguments, 0);
    if (!callback.IsObject() || !callback.object->IsCallable()) {
      return call.Throw("TypeError", "the callback provided is not a function");
    }
    const Value receiver = Argument(call.arguments, 1);
    const std::vector<Value> tokens = snapshot(call);
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      const js::Result step = call.interpreter.CallFunction(
          callback, receiver,
          {tokens[index], Value::Number(static_cast<double>(index)), call.self});
      if (step.IsAbrupt()) {
        return call.ThrowValue(step.value);
      }
    }
    return Value::Undefined();
  });
  method("toString", [](NativeCall& call) {
    const ListTarget target = TargetOf(call.self);
    const std::string* text = AttributeText(target);
    return Value::String(text == nullptr ? std::string() : *text);
  });

  return prototype;
}

void DomBindings::UpdateTokenList(dom::Element& element, const std::string& attribute,
                                 const std::vector<std::string>& tokens) {
  if (element.GetAttribute(attribute) == nullptr && tokens.empty()) {
    return;
  }
  SetElementAttribute(element, attribute, Serialize(tokens));
}

js::Value DomBindings::MakeTokenList(dom::Element& element, const char* attribute) {
  const Value prototype = TokenListInterface();
  const Value target = interpreter_->NewObjectValue();
  if (!target.IsObject()) {
    return target;
  }
  if (prototype.IsObject()) {
    target.object->SetPrototype(prototype.object);
  }
  // Hidden, unlike the same slot on a node wrapper: a wrapper's internals are
  // reached only through a get trap that never asks for them, but this object
  // *is* the proxy's target, so `Object.keys(el.classList)` and `for...in` walk
  // it directly -- and without this they answer with `#node` and
  // `#tokenAttribute` rather than with the token indices.
  target.object->SetHidden(kNodeSlot, PointerValue(&element));
  target.object->SetHidden(kTokenAttributeSlot, Value::String(attribute));

  const Value handler = interpreter_->NewObjectValue();
  if (!handler.IsObject()) {
    return target;
  }
  const auto trap = [this, &handler](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      handler.object->Set(name, native);
    }
  };

  // The indexed getter, and the only reason this is a Proxy at all. An index
  // past the end is `undefined` -- distinct from `item()`, which answers
  // `null` for the same argument, because one is a property access and the
  // other is an operation with a nullable return.
  trap("get", [](NativeCall& call) -> Value {
    const Value proxy_target = Argument(call.arguments, 0);
    const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
    if (!key.IsSymbol()) {
      if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
        const std::vector<std::string> tokens = TokensOf(TargetOf(proxy_target));
        return index < tokens.size() ? Value::String(tokens[index]) : Value::Undefined();
      }
    }
    return call.interpreter.GetPropertyValue(proxy_target, key);
  });
  trap("has", [](NativeCall& call) -> Value {
    const Value proxy_target = Argument(call.arguments, 0);
    const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
    if (!key.IsSymbol()) {
      if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
        return Value::Bool(index < TokensOf(TargetOf(proxy_target)).size());
      }
    }
    return Value::Bool(proxy_target.IsObject() && proxy_target.object->GetProperty(key) != nullptr);
  });

  js::Value* constructor = interpreter_->GlobalScope()->Lookup("Proxy");
  if (constructor == nullptr || !constructor->IsObject()) {
    return target;
  }
  const js::Result made =
      interpreter_->CallFunction(*constructor, Value::Undefined(), {target, handler});
  return made.IsAbrupt() ? target : made.value;
}

}  // namespace microbrowser::bindings
