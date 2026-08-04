#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <string>
#include <vector>

// `element.style`.
//
// Backed by the `style` attribute rather than by a parsed copy, because the
// attribute is the state: the cascade reads it, `setAttribute` can rewrite it,
// and a copy held here would go stale the moment either did.
//
// Built as a `Proxy` over the element, which is the one mechanism in the engine
// that can answer for a property name nobody enumerated in advance -- and there
// are a few hundred CSS properties. Reusing it rather than listing the common
// ones means `el.style.gridTemplateColumns` works the day grid does.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// `backgroundColor` is `background-color`. The one rule that turns a
// JavaScript property name into a CSS one, and the same rule `dataset` uses in
// the other direction.
std::string ToCssName(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back('-');
      out.push_back(static_cast<char>(c - 'A' + 'a'));
      continue;
    }
    out.push_back(c);
  }
  return out;
}

// The declarations in a `style` attribute, in order. Parsed on every access
// rather than kept, for the reason at the top of the file.
std::vector<std::pair<std::string, std::string>> Parse(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> declarations;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = std::min(text.find(';', at), text.size());
    const std::string_view piece(text.data() + at, end - at);
    const std::size_t colon = piece.find(':');
    if (colon != std::string_view::npos) {
      const auto trim = [](std::string_view value) {
        const std::size_t begin = value.find_first_not_of(" \t\n");
        if (begin == std::string_view::npos) {
          return std::string();
        }
        return std::string(value.substr(begin, value.find_last_not_of(" \t\n") - begin + 1));
      };
      const std::string name = trim(piece.substr(0, colon));
      if (!name.empty()) {
        declarations.emplace_back(name, trim(piece.substr(colon + 1)));
      }
    }
    at = end + 1;
  }
  return declarations;
}

std::string Serialize(const std::vector<std::pair<std::string, std::string>>& declarations) {
  std::string out;
  for (const auto& declaration : declarations) {
    if (declaration.second.empty()) {
      continue;  // an empty value removes the property, which is what `= ''` means
    }
    if (!out.empty()) {
      out += "; ";
    }
    out += declaration.first + ": " + declaration.second;
  }
  return out;
}

}  // namespace

js::Value DomBindings::MakeStyle(dom::Element& element) {
  // A Proxy needs a target, and the target is the element's wrapper -- so the
  // traps can find the element the same way every other binding does.
  const Value target = interpreter_->NewObjectValue();
  if (!target.IsObject()) {
    return target;
  }
  target.object->Set(kNodeSlot, PointerValue(&element));

  const Value handler = interpreter_->NewObjectValue();
  if (!handler.IsObject()) {
    return handler;
  }
  const auto trap = [this, &handler](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      handler.object->Set(name, native);
    }
  };

  trap("get", [](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Undefined();  // no protocol hooks on a style declaration
    }
    // Checked before the conversion, or `cssText` becomes `css-text` and
    // stops being the one name here that is not a CSS property.
    const std::string written = js::ToString(key);
    const std::string* text = static_cast<dom::Element*>(self)->GetAttribute("style");
    if (written == "cssText") {
      return Value::String(text == nullptr ? std::string() : *text);
    }
    const std::string wanted = ToCssName(written);
    for (const auto& declaration : Parse(text == nullptr ? std::string() : *text)) {
      if (declaration.first == wanted) {
        return Value::String(declaration.second);
      }
    }
    // An unset property is the empty string, not undefined. A page tests
    // `if (el.style.display === 'none')` and both answers have to be strings
    // or the comparison is wrong in a way nothing reports.
    return Value::String(std::string());
  });

  trap("set", [](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Bool(true);
    }
    auto& target_element = static_cast<dom::Element&>(*self);
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Bool(true);
    }
    const std::string written = js::ToString(key);
    const std::string value = js::ToString(Argument(call.arguments, 2));
    if (written == "cssText") {
      target_element.SetAttribute("style", value);
      return Value::Bool(true);
    }
    const std::string wanted = ToCssName(written);
    const std::string* text = target_element.GetAttribute("style");
    std::vector<std::pair<std::string, std::string>> declarations =
        Parse(text == nullptr ? std::string() : *text);
    bool replaced = false;
    for (auto& declaration : declarations) {
      if (declaration.first == wanted) {
        // Rewritten in place rather than appended, so setting a property twice
        // leaves one declaration and keeps the order a page wrote.
        declaration.second = value;
        replaced = true;
      }
    }
    if (!replaced) {
      declarations.emplace_back(wanted, value);
    }
    target_element.SetAttribute("style", Serialize(declarations));
    return Value::Bool(true);
  });

  // Through the language's own `Proxy`, rather than a second implementation of
  // one. The constructor is read from the global scope where the engine
  // installed it.
  js::Value* constructor = interpreter_->GlobalScope()->Lookup("Proxy");
  if (constructor == nullptr || !constructor->IsObject()) {
    return Value::Undefined();
  }
  const js::Result made = interpreter_->CallFunction(*constructor, Value::Undefined(),
                                                     {target, handler});
  return made.IsAbrupt() ? Value::Undefined() : made.value;
}

}  // namespace microbrowser::bindings
