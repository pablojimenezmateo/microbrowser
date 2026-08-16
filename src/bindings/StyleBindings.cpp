#include "bindings/BindingSupport.h"
#include "bindings/CssomInternals.h"
#include "css/DeclarationText.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"

#include <string>
#include <vector>

#include "dom/Node.h"
#include "util/StringUtil.h"

// `element.style` and `element.dataset`.
//
// Style is backed by the `style` attribute rather than by a parsed copy, because the
// attribute is the state: the cascade reads it, `setAttribute` can rewrite it,
// and a copy held here would go stale the moment either did.
//
// Built as a `Proxy` over the element, which is the one mechanism in the engine
// that can answer for a property name nobody enumerated in advance -- and there
// are a few hundred CSS properties. Reusing it rather than listing the common
// ones means `el.style.gridTemplateColumns` works the day grid does.
//
// `setProperty` / `removeProperty` / `getPropertyValue` are real methods on the
// target, returned by name from the get trap. Without them the trap answers
// every unknown name as a CSS property and returns the empty string, so
// `typeof style.setProperty === "string"` and ShadyCSS's `style.setProperty(...)`
// becomes "X is not a function". youtube.com hits that on every styled
// component.
//
// Dataset is the same Proxy shape for `data-*`: a write must reach the attribute
// table, or youtube's `movie_player.dataset.version = jsUrl` is a no-op and the
// player proxy's version check clears the stamp.

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

// A name that is CSSOM, not a CSS property. Checked before ToCssName, or
// `cssText` becomes `css-text` and `setProperty` becomes an empty string that
// a page then tries to call.
bool IsCssomName(const std::string& name) {
  return name == "cssText" || name == "setProperty" || name == "removeProperty" ||
         name == "getPropertyValue" || name == "getPropertyPriority" || name == "length" ||
         name == "item" || name == "parentRule" || name == "cssFloat";
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
  if (!out.empty()) {
    out += ';';
  }
  return out;
}

void PutDeclaration(dom::Element& element, const std::string& css_name, std::string value) {
  const std::string* text = element.GetAttribute("style");
  std::vector<std::pair<std::string, std::string>> declarations =
      Parse(text == nullptr ? std::string() : *text);
  bool replaced = false;
  for (auto& declaration : declarations) {
    if (declaration.first == css_name) {
      declaration.second = std::move(value);
      replaced = true;
    }
  }
  if (!replaced) {
    declarations.emplace_back(css_name, std::move(value));
  }
  element.SetAttribute("style", Serialize(declarations));
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
      native.object->Set(kOwnerSlot, OwnerValue(this));
      handler.object->Set(name, native);
    }
  };

  trap("get", [](NativeCall& call) {
    const Value receiver_target = Argument(call.arguments, 0);
    dom::Node* self = NodeOf(receiver_target);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return call.interpreter.GetPropertyValue(Argument(call.arguments, 0),
                                               KeyOfTrapArgument(key));
    }
    // Checked before the conversion, or `cssText` becomes `css-text` and
    // stops being the one name here that is not a CSS property.
    const std::string written = js::ToString(key);
    if (IsCssomName(written)) {
      return call.interpreter.GetPropertyValue(receiver_target, KeyOfTrapArgument(key));
    }
    const std::string* text = static_cast<dom::Element*>(self)->GetAttribute("style");
    const std::string wanted = ToCssName(written);
    for (const auto& declaration : Parse(text == nullptr ? std::string() : *text)) {
      if (declaration.first == wanted) {
        return Value::String(declaration.second);
      }
    }
    const Value inherited =
        call.interpreter.GetPropertyValue(receiver_target, KeyOfTrapArgument(key));
    if (!inherited.IsUndefined()) {
      return inherited;
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
    if (IsCssomName(written) && written != "cssText" && written != "cssFloat") {
      return Value::Bool(true);
    }
    if (written == "cssText" || written == "cssFloat") {
      const js::Result assigned = call.interpreter.SetProperty(
          Argument(call.arguments, 0), KeyOfTrapArgument(key), Argument(call.arguments, 2));
      if (assigned.IsAbrupt()) {
        return call.ThrowValue(assigned.value);
      }
      return Value::Bool(true);
    }
    bool any_upper = false;
    bool any_lower = false;
    for (const char c : written) {
      if (c >= 'A' && c <= 'Z') {
        any_upper = true;
      } else if (c >= 'a' && c <= 'z') {
        any_lower = true;
      }
    }
    if (any_upper && !any_lower) {
      return Value::Bool(true);
    }
    // **CSSOM parses before it stores, and serializes on the way out.** An assignment that does not
    // parse does *nothing* -- `el.style.color = 'nonsense'` leaves the declaration as it was -- and
    // a valid one reads back canonically, so `#000` becomes `rgb(0, 0, 0)`. This engine stored the
    // text and handed it back, which is wrong in both directions and invisible until something asks.
    // `css/CSS2/syntax/colors-007.html` is 904 subtests of exactly that.
    //
    // `Unknown` is a property whose grammar `src/css` does not check, and it keeps the old
    // behaviour deliberately: a property wrongly canonicalised silently changes what a page reads,
    // where one left alone behaves as it always did.
    const std::string property = ToCssName(written);
    const std::string raw_value = js::ToString(Argument(call.arguments, 2));
    const std::string value(util::TrimAscii(raw_value));
    std::string canonical;
    switch (css::CanonicaliseDeclaration(property, value, &canonical)) {
      case css::DeclarationValidity::Invalid:
        return Value::Bool(true);
      case css::DeclarationValidity::Canonical:
        PutDeclaration(target_element, property, canonical);
        return Value::Bool(true);
      case css::DeclarationValidity::Unknown:
        if (!CssomKeepsUnknownDeclaration(property, value)) {
          return Value::Bool(true);
        }
        break;
    }
    PutDeclaration(target_element, property, value);
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
  const Value style = made.IsAbrupt() ? Value::Undefined() : made.value;
  if (style.IsObject() && interfaces_.IsObject()) {
    const Value* proto = interfaces_.object->GetOwn("CSSStyleProperties");
    if (proto == nullptr || !proto->IsObject()) {
      proto = interfaces_.object->GetOwn("CSSStyleDeclaration");
    }
    if (proto != nullptr && proto->IsObject()) {
      style.object->SetPrototype(proto->object);
      target.object->SetPrototype(proto->object);
    }
  }
  return style;
}

js::Value DomBindings::MakeDataset(dom::Element& element) {
  // Same Proxy shape as `style`: a page writes `el.dataset.foo = 'bar'` and
  // that must become `data-foo` on the element, not a property on a throwaway
  // object. The previous snapshot implementation dropped every write.
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
      native.object->Set(kOwnerSlot, OwnerValue(this));
      handler.object->Set(name, native);
    }
  };

  const auto to_data_attr = [](const std::string& camel) {
    std::string out = "data-";
    for (const char c : camel) {
      if (c >= 'A' && c <= 'Z') {
        out.push_back('-');
        out.push_back(static_cast<char>(c - 'A' + 'a'));
      } else {
        out.push_back(c);
      }
    }
    return out;
  };

  trap("get", [to_data_attr](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Undefined();
    }
    const std::string written = js::ToString(key);
    if (written == "toJSON" || written == "toString" || written == "valueOf") {
      return Value::Undefined();
    }
    const std::string* found =
        static_cast<dom::Element*>(self)->GetAttribute(to_data_attr(written));
    return found == nullptr ? Value::Undefined() : Value::String(*found);
  });

  trap("set", [to_data_attr](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Bool(true);
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Bool(true);
    }
    owner->SetElementAttribute(static_cast<dom::Element&>(*self),
                               to_data_attr(js::ToString(key)),
                               js::ToString(Argument(call.arguments, 2)));
    return Value::Bool(true);
  });

  trap("has", [to_data_attr](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Bool(false);
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Bool(false);
    }
    return Value::Bool(static_cast<dom::Element*>(self)->GetAttribute(
                           to_data_attr(js::ToString(key))) != nullptr);
  });

  trap("deleteProperty", [to_data_attr](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Bool(true);
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Bool(true);
    }
    static_cast<dom::Element*>(self)->RemoveAttribute(to_data_attr(js::ToString(key)));
    return Value::Bool(true);
  });

  trap("ownKeys", [](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    std::vector<Value> keys;
    if (self != nullptr && self->IsElement()) {
      for (const dom::Attribute& attribute :
           static_cast<dom::Element*>(self)->Attributes()) {
        if (attribute.name.rfind("data-", 0) != 0) {
          continue;
        }
        std::string name;
        bool upper = false;
        for (const char c : attribute.name.substr(5)) {
          if (c == '-') {
            upper = true;
            continue;
          }
          name.push_back(upper ? util::detail::AsciiToUpper(c) : c);
          upper = false;
        }
        keys.push_back(Value::String(std::move(name)));
      }
    }
    return call.interpreter.NewArrayValue(std::move(keys));
  });

  trap("getOwnPropertyDescriptor", [to_data_attr](NativeCall& call) {
    dom::Node* self = NodeOf(Argument(call.arguments, 0));
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const Value key = Argument(call.arguments, 1);
    if (key.IsSymbol()) {
      return Value::Undefined();
    }
    const std::string* found =
        static_cast<dom::Element*>(self)->GetAttribute(to_data_attr(js::ToString(key)));
    if (found == nullptr) {
      return Value::Undefined();
    }
    const Value descriptor = call.interpreter.NewObjectValue();
    if (!descriptor.IsObject()) {
      return Value::Undefined();
    }
    descriptor.object->Set("value", Value::String(*found));
    descriptor.object->Set("writable", Value::Bool(true));
    descriptor.object->Set("enumerable", Value::Bool(true));
    descriptor.object->Set("configurable", Value::Bool(true));
    return descriptor;
  });

  js::Value* constructor = interpreter_->GlobalScope()->Lookup("Proxy");
  if (constructor == nullptr || !constructor->IsObject()) {
    return Value::Undefined();
  }
  const js::Result made =
      interpreter_->CallFunction(*constructor, Value::Undefined(), {target, handler});
  return made.IsAbrupt() ? Value::Undefined() : made.value;
}

}  // namespace microbrowser::bindings
