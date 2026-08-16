#include "bindings/CssomInternals.h"

#include "bindings/WebIdl.h"
#include "css/DeclarationText.h"
#include "css/StyleSheet.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {
namespace {

using js::NativeCall;
using js::Value;

bool IsComputedStyle(const Value& style) {
  js::Object* raw = style.IsObject() ? BehindProxies(style.object) : nullptr;
  return raw != nullptr && raw->GetOwn(kComputedStyleSlot) != nullptr;
}

dom::Element* StyleElement(const Value& style) {
  dom::Node* node = NodeOf(style);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

std::vector<css::Declaration> ReadDeclarations(const Value& style) {
  const Value* sheet = CssomSheetSlotOf(style);
  if (sheet == nullptr) {
    return {};
  }
  std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
  const css::CssomRule* rule = LocateCssomRule(rules, style);
  return rule == nullptr ? std::vector<css::Declaration>{} : rule->declarations;
}

std::string SerializeCssText(const std::vector<css::Declaration>& declarations) {
  return css::SerializeCssDeclarationBlock(declarations);
}

std::vector<css::Declaration> ElementDeclarations(dom::Element& element) {
  const std::string* text = element.GetAttribute("style");
  std::vector<css::Declaration> declarations =
      css::ParseDeclarationList(text == nullptr ? "" : *text);
  std::vector<css::Declaration> kept;
  kept.reserve(declarations.size());
  for (css::Declaration& declaration : declarations) {
    std::string canonical;
    switch (css::CanonicaliseDeclaration(declaration.property, declaration.value, &canonical)) {
      case css::DeclarationValidity::Invalid:
        continue;
      case css::DeclarationValidity::Canonical:
        declaration.value = std::move(canonical);
        break;
      case css::DeclarationValidity::Unknown:
        if (!canonical.empty()) {
          declaration.value = std::move(canonical);
        }
        break;
    }
    kept.push_back(std::move(declaration));
  }
  return kept;
}

std::string PageCssName(std::string_view name) {
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

}  // namespace

void InstallCssomStylePrototype(js::Interpreter& interpreter, const js::Value& interfaces,
                                const js::Value& owner,
                                std::function<void(const js::Value&, std::string)> write_sheet,
                                GeometrySource* geometry) {
  if (!interfaces.IsObject()) {
    return;
  }
  const Value* proto = interfaces.object->GetOwn("CSSStyleDeclaration");
  if (proto == nullptr || !proto->IsObject()) {
    return;
  }
  const Value css_style_declaration = *proto;

  const auto write_declarations = [write_sheet](const Value& style,
                                                std::vector<css::Declaration> declarations) {
    const Value* sheet = CssomSheetSlotOf(style);
    if (sheet == nullptr) {
      return;
    }
    std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    css::CssomRule* parsed = LocateCssomRule(rules, style);
    if (parsed == nullptr) {
      return;
    }
    parsed->declarations = std::move(declarations);
    for (css::CssomRule& top : rules) {
      css::RefreshCssomCssText(top);
    }
    write_sheet(*sheet, css::JoinCssomRules(rules));
  };

  const auto style_method = [&interpreter, &css_style_declaration, &owner](
                                const char* name, double arity, js::NativeFunction function) {
    const Value native = interpreter.NewNativeValue(name, std::move(function));
    if (!native.IsObject()) {
      return;
    }
    native.object->Set(kOwnerSlot, owner);
    SetFunctionLength(native, arity);
    css_style_declaration.object->Set(name, native);
  };

  const auto accessor = [&interpreter, &owner](const Value& target, const char* name,
                                               js::NativeFunction getter,
                                               js::NativeFunction setter = nullptr) {
    if (!target.IsObject()) {
      return;
    }
    const std::string get_name = std::string("get ") + name;
    const Value get = interpreter.NewNativeValue(get_name.c_str(), std::move(getter));
    if (!get.IsObject()) {
      return;
    }
    get.object->Set(kOwnerSlot, owner);
    SetFunctionLength(get, 0);
    js::Object* set = nullptr;
    if (setter) {
      const std::string set_name = std::string("set ") + name;
      const Value native = interpreter.NewNativeValue(set_name.c_str(), std::move(setter));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, owner);
        SetFunctionLength(native, 1);
        set = native.object;
      }
    }
    target.object->DefineAccessor(name, get.object, set);
  };

  style_method("getPropertyValue", 1, [geometry](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleDeclaration", "getPropertyValue", 1)) {
      return call.ThrownValue();
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    if (IsComputedStyle(call.self)) {
      dom::Element* element = StyleElement(call.self);
      if (element == nullptr || geometry == nullptr) {
        return Value::String("");
      }
      return Value::String(geometry->QueryUsedValue(*element, wanted).value_or(std::string()));
    }
    if (dom::Element* element = StyleElement(call.self)) {
      const std::vector<css::Declaration> declarations = ElementDeclarations(*element);
      for (css::Declaration declaration : declarations) {
        if (declaration.property != wanted) {
          continue;
        }
        std::string canonical;
        switch (css::CanonicaliseDeclaration(declaration.property, declaration.value,
                                             &canonical)) {
          case css::DeclarationValidity::Invalid:
            return Value::String("");
          case css::DeclarationValidity::Canonical:
            return Value::String(std::move(canonical));
          case css::DeclarationValidity::Unknown:
            return Value::String(canonical.empty() ? declaration.value : std::move(canonical));
        }
      }
      return Value::String(css::SpecifiedShorthandValue(wanted, declarations));
    }
    const std::vector<css::Declaration> declarations = ReadDeclarations(call.self);
    for (const css::Declaration& declaration : declarations) {
      if (declaration.property == wanted) {
        return Value::String(declaration.value);
      }
    }
    return Value::String(css::SpecifiedShorthandValue(wanted, declarations));
  });
  style_method("getPropertyPriority", 1, [](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleDeclaration", "getPropertyPriority", 1)) {
      return call.ThrownValue();
    }
    if (IsComputedStyle(call.self)) {
      return Value::String("");
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    if (dom::Element* element = StyleElement(call.self)) {
      for (const css::Declaration& declaration : ElementDeclarations(*element)) {
        if (declaration.property == wanted) {
          return Value::String(declaration.important ? "important" : "");
        }
      }
      return Value::String("");
    }
    for (const css::Declaration& declaration : ReadDeclarations(call.self)) {
      if (declaration.property == wanted) {
        return Value::String(declaration.important ? "important" : "");
      }
    }
    return Value::String("");
  });
  style_method("setProperty", 2, [write_declarations](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleDeclaration", "setProperty", 2)) {
      return call.ThrownValue();
    }
    if (IsComputedStyle(call.self)) {
      return ThrowDom(call, "NoModificationAllowedError", "computed style is read-only");
    }
    std::string name = js::ToString(Argument(call.arguments, 0));
    const std::string raw_value = js::ToString(Argument(call.arguments, 1));
    std::string value(util::TrimAscii(raw_value));
    const bool important =
        util::AsciiLowerCase(js::ToString(Argument(call.arguments, 2))) == "important";
    std::string canonical;
    switch (css::CanonicaliseDeclaration(name, value, &canonical)) {
      case css::DeclarationValidity::Invalid:
        return Value::Undefined();
      case css::DeclarationValidity::Canonical:
        value = std::move(canonical);
        break;
      case css::DeclarationValidity::Unknown:
        if (!CssomKeepsUnknownDeclaration(name, value)) {
          return Value::Undefined();
        }
        if (!canonical.empty()) {
          value = std::move(canonical);
        }
        break;
    }
    if (dom::Element* element = StyleElement(call.self)) {
      std::vector<css::Declaration> declarations = ElementDeclarations(*element);
      bool replaced = false;
      for (css::Declaration& declaration : declarations) {
        if (declaration.property == name) {
          declaration.value = value;
          declaration.important = important;
          replaced = true;
        }
      }
      if (!replaced && !value.empty()) {
        css::Declaration added;
        added.property = std::move(name);
        added.value = std::move(value);
        added.important = important;
        declarations.push_back(std::move(added));
      }
      element->SetAttribute("style", SerializeCssText(declarations));
      return Value::Undefined();
    }
    std::vector<css::Declaration> declarations = ReadDeclarations(call.self);
    bool replaced = false;
    for (css::Declaration& declaration : declarations) {
      if (declaration.property == name) {
        declaration.value = std::move(value);
        declaration.important = important;
        replaced = true;
      }
    }
    if (!replaced && !value.empty()) {
      css::Declaration added;
      added.property = std::move(name);
      added.value = std::move(value);
      added.important = important;
      declarations.push_back(std::move(added));
    }
    write_declarations(call.self, std::move(declarations));
    return Value::Undefined();
  });
  style_method("removeProperty", 1, [write_declarations](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleDeclaration", "removeProperty", 1)) {
      return call.ThrownValue();
    }
    if (IsComputedStyle(call.self)) {
      return ThrowDom(call, "NoModificationAllowedError", "computed style is read-only");
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    if (dom::Element* element = StyleElement(call.self)) {
      std::vector<css::Declaration> declarations = ElementDeclarations(*element);
      std::string previous;
      std::vector<css::Declaration> kept;
      for (css::Declaration& declaration : declarations) {
        if (declaration.property == wanted) {
          previous = std::move(declaration.value);
          continue;
        }
        kept.push_back(std::move(declaration));
      }
      element->SetAttribute("style", SerializeCssText(kept));
      return Value::String(std::move(previous));
    }
    std::vector<css::Declaration> declarations = ReadDeclarations(call.self);
    std::string previous;
    std::vector<css::Declaration> kept;
    for (css::Declaration& declaration : declarations) {
      if (declaration.property == wanted) {
        previous = std::move(declaration.value);
        continue;
      }
      kept.push_back(std::move(declaration));
    }
    write_declarations(call.self, std::move(kept));
    return Value::String(std::move(previous));
  });
  style_method("item", 1, [](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleDeclaration", "item", 1)) {
      return call.ThrownValue();
    }
    std::vector<css::Declaration> declarations;
    if (dom::Element* element = StyleElement(call.self);
        element != nullptr && !IsComputedStyle(call.self)) {
      declarations = ElementDeclarations(*element);
    } else {
      declarations = ReadDeclarations(call.self);
    }
    const double n = js::ToNumber(Argument(call.arguments, 0));
    if (n < 0.0 || n >= static_cast<double>(declarations.size())) {
      return Value::String("");
    }
    return Value::String(declarations[static_cast<std::size_t>(n)].property);
  });

  accessor(css_style_declaration, "cssText",
           [](NativeCall& call) {
             if (!IsCssomStyleThis(call.self)) {
               return call.Throw("TypeError", "Illegal invocation");
             }
             if (IsComputedStyle(call.self)) {
               return Value::String("");
             }
             if (dom::Element* element = StyleElement(call.self)) {
               return Value::String(SerializeCssText(ElementDeclarations(*element)));
             }
             return Value::String(SerializeCssText(ReadDeclarations(call.self)));
           },
           [write_declarations](NativeCall& call) -> Value {
             if (!IsCssomStyleThis(call.self)) {
               return call.Throw("TypeError", "Illegal invocation");
             }
             if (IsComputedStyle(call.self)) {
               return ThrowDom(call, "NoModificationAllowedError", "computed style is read-only");
             }
             std::vector<css::Declaration> parsed =
                 css::ParseDeclarationList(js::ToString(Argument(call.arguments, 0)));
             std::vector<css::Declaration> kept;
             for (css::Declaration& declaration : parsed) {
               std::string canonical;
               switch (css::CanonicaliseDeclaration(declaration.property, declaration.value,
                                                    &canonical)) {
                 case css::DeclarationValidity::Invalid:
                   continue;
                 case css::DeclarationValidity::Canonical:
                   declaration.value = std::move(canonical);
                   break;
                 case css::DeclarationValidity::Unknown:
                   if (!CssomKeepsUnknownDeclaration(declaration.property, declaration.value)) {
                     continue;
                   }
                   if (!canonical.empty()) {
                     declaration.value = std::move(canonical);
                   }
                   break;
               }
               kept.push_back(std::move(declaration));
             }
             if (dom::Element* element = StyleElement(call.self)) {
               element->SetAttribute("style", SerializeCssText(kept));
               return Value::Undefined();
             }
             write_declarations(call.self, std::move(kept));
             return Value::Undefined();
           });
  accessor(css_style_declaration, "length", [](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (dom::Element* element = StyleElement(call.self);
        element != nullptr && !IsComputedStyle(call.self)) {
      return Value::Number(static_cast<double>(ElementDeclarations(*element).size()));
    }
    if (IsComputedStyle(call.self)) {
      return Value::Number(1);
    }
    return Value::Number(static_cast<double>(ReadDeclarations(call.self).size()));
  });
  accessor(css_style_declaration, "parentRule", [](NativeCall& call) -> Value {
    if (!IsCssomStyleThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    js::Object* raw = call.self.IsObject() ? BehindProxies(call.self.object) : nullptr;
    if (raw == nullptr) {
      return Value::Null();
    }
    const Value* parent = raw->GetOwn(kCssomCssTextSlot);
    return parent != nullptr && parent->IsObject() ? *parent : Value::Null();
  });
  if (const Value* props = interfaces.object->GetOwn("CSSStyleProperties");
      props != nullptr && props->IsObject()) {
    accessor(*props, "cssFloat",
             [](NativeCall& call) -> Value {
               if (!IsCssomStyleThis(call.self)) {
                 return call.Throw("TypeError", "Illegal invocation");
               }
               const Value* method =
                   call.self.IsObject() ? call.self.object->Get("getPropertyValue") : nullptr;
               if (method == nullptr || !method->IsObject()) {
                 return Value::String("");
               }
               const js::Result result = call.interpreter.CallFunction(
                   *method, call.self, {Value::String("float")});
               return result.IsAbrupt() ? call.ThrowValue(result.value) : result.value;
             },
             [](NativeCall& call) -> Value {
               if (!IsCssomStyleThis(call.self)) {
                 return call.Throw("TypeError", "Illegal invocation");
               }
               const js::Result assigned = call.interpreter.SetProperty(
                   call.self, js::PropertyKey("float"), Argument(call.arguments, 0));
               if (assigned.IsAbrupt()) {
                 return call.ThrowValue(assigned.value);
               }
               return Value::Undefined();
             });
  }

  if (js::Object* iterator_symbol = interpreter.SymbolIterator()) {
    const Value iterate = interpreter.NewNativeValue("[Symbol.iterator]", [](NativeCall& call) -> Value {
      std::vector<Value> names;
      if (IsCssomStyleThis(call.self)) {
        std::vector<css::Declaration> declarations;
        if (dom::Element* element = StyleElement(call.self);
            element != nullptr && !IsComputedStyle(call.self)) {
          declarations = ElementDeclarations(*element);
        } else {
          declarations = ReadDeclarations(call.self);
        }
        names.reserve(declarations.size());
        for (const css::Declaration& declaration : declarations) {
          names.push_back(Value::String(declaration.property));
        }
      }
      const Value array = call.interpreter.NewArrayValue(std::move(names));
      if (!array.IsObject()) {
        return array;
      }
      const Value* values_iter =
          array.object->Get(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
      if (values_iter == nullptr || !values_iter->IsObject()) {
        return array;
      }
      const js::Result walked = call.interpreter.CallFunction(*values_iter, array, {});
      return walked.IsAbrupt() ? call.ThrowValue(walked.value) : walked.value;
    });
    if (iterate.IsObject()) {
      css_style_declaration.object->Set(js::PropertyKey::Symbol(iterator_symbol), iterate);
    }
  }

  if (const Value* page = interfaces.object->GetOwn("CSSPageDescriptors");
      page != nullptr && page->IsObject()) {
    for (const char* name :
         {"bleed", "marks", "size", "pageOrientation", "page-orientation", "margin", "marginTop",
          "marginRight", "marginBottom", "marginLeft", "margin-top", "margin-right",
          "margin-bottom", "margin-left"}) {
      accessor(*page, name,
               [name](NativeCall& call) -> Value {
                 js::Object* raw = call.self.IsObject() ? BehindProxies(call.self.object) : nullptr;
                 if (raw == nullptr || raw->GetOwn(kCssomPageStyleSlot) == nullptr) {
                   return call.Throw("TypeError", "Illegal invocation");
                 }
                 const Value* method =
                     call.self.IsObject() ? call.self.object->Get("getPropertyValue") : nullptr;
                 if (method == nullptr || !method->IsObject()) {
                   return Value::String("");
                 }
                 const js::Result result = call.interpreter.CallFunction(
                     *method, call.self, {Value::String(PageCssName(name))});
                 return result.IsAbrupt() ? call.ThrowValue(result.value) : result.value;
               },
               [name](NativeCall& call) -> Value {
                 js::Object* raw = call.self.IsObject() ? BehindProxies(call.self.object) : nullptr;
                 if (raw == nullptr || raw->GetOwn(kCssomPageStyleSlot) == nullptr) {
                   return call.Throw("TypeError", "Illegal invocation");
                 }
                 const js::Result assigned = call.interpreter.SetProperty(
                     call.self, js::PropertyKey(PageCssName(name)), Argument(call.arguments, 0));
                 if (assigned.IsAbrupt()) {
                   return call.ThrowValue(assigned.value);
                 }
                 return Value::Undefined();
               });
    }
  }
}

}  // namespace microbrowser::bindings
