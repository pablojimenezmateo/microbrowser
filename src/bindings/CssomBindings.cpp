// `cssRules`, `insertRule`, `deleteRule`, and `CSSStyleRule.cssText`.
//
// The cascade still flattens `@media` at parse time (TD-0002). This list does
// not: a short cssRules would be a wrong answer (ADR 0012). Linked sheets read
// the text the engine stamped on the `<link>` when the bytes arrived.

#include "bindings/DomBindings.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "css/Cssom.h"
#include "dom/Node.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

using SheetStorage = std::shared_ptr<std::string>;

SheetStorage* SheetStoragePtr(const Value& sheet) {
  if (!sheet.IsObject()) {
    return nullptr;
  }
  const Value* marker = sheet.object->GetOwn(kCSSStyleSheetMarkerSlot);
  if (marker == nullptr || !js::ToBoolean(*marker)) {
    return nullptr;
  }
  const Value* slot = sheet.object->GetOwn(kCSSSheetStorageSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<SheetStorage*>(static_cast<std::uintptr_t>(slot->number));
}

dom::Element* SheetOwnerOf(const Value& sheet) {
  if (!sheet.IsObject()) {
    return nullptr;
  }
  const Value* slot = sheet.object->GetOwn(kSheetOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Element*>(static_cast<std::uintptr_t>(slot->number));
}

std::string DirectText(const dom::Element& element) {
  std::string text;
  for (const std::unique_ptr<dom::Node>& child : element.Children()) {
    if (child->IsText()) {
      text += static_cast<const dom::Text&>(*child).Data();
    }
  }
  return text;
}

std::string SheetText(const Value& sheet) {
  if (SheetStorage* storage = SheetStoragePtr(sheet); storage != nullptr && *storage != nullptr) {
    return **storage;
  }
  const dom::Element* owner = SheetOwnerOf(sheet);
  if (owner == nullptr) {
    return {};
  }
  if (owner->TagName() == "style") {
    return DirectText(*owner);
  }
  if (const std::string* text = owner->LinkedStyleSheetText()) {
    return *text;
  }
  return {};
}

std::string JoinCssom(const std::vector<css::CssomRule>& rules) {
  std::string out;
  for (const css::CssomRule& rule : rules) {
    if (!out.empty()) {
      out += '\n';
    }
    out += rule.css_text;
  }
  return out;
}

}  // namespace

void DomBindings::InstallCssomSheetRules() {
  EnsureInterfaces();
  if (interpreter_ == nullptr || !interfaces_.IsObject()) {
    return;
  }
  const Value* sheet_proto = interfaces_.object->GetOwn("StyleSheet");
  if (sheet_proto == nullptr || !sheet_proto->IsObject()) {
    return;
  }

  const Value rule_proto = interpreter_->NewObjectValue();
  if (rule_proto.IsObject()) {
    interfaces_.object->Set("CSSStyleRule", rule_proto);
    const auto rule_accessor = [this, &rule_proto](const char* name, js::NativeFunction getter) {
      const Value native = interpreter_->NewNativeValue(name, std::move(getter));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, OwnerValue(this));
        rule_proto.object->DefineAccessor(name, native.object, nullptr);
      }
    };
    rule_accessor("type", [](NativeCall& call) -> Value {
      const Value* slot = call.self.IsObject() ? call.self.object->GetOwn(kCssomTypeSlot) : nullptr;
      return slot == nullptr ? Value::Number(0) : *slot;
    });
    rule_accessor("cssText", [](NativeCall& call) -> Value {
      const Value* slot =
          call.self.IsObject() ? call.self.object->GetOwn(kCssomCssTextSlot) : nullptr;
      return slot == nullptr ? Value::String("") : *slot;
    });
    rule_accessor("parentRule", [](NativeCall&) { return Value::Null(); });
  }

  const auto make_rule = [this, rule_proto](const css::CssomRule& parsed) -> Value {
    const Value rule = interpreter_->NewObjectValue();
    if (!rule.IsObject()) {
      return rule;
    }
    if (rule_proto.IsObject()) {
      rule.object->SetPrototype(rule_proto.object);
    }
    rule.object->SetHidden(kCssomTypeSlot, Value::Number(static_cast<double>(parsed.type)));
    rule.object->SetHidden(kCssomCssTextSlot, Value::String(parsed.css_text));
    rule.object->Set(kOwnerSlot, OwnerValue(this));
    return rule;
  };

  const auto set_sheet_text = [this](const Value& sheet, std::string text) {
    if (SheetStorage* storage = SheetStoragePtr(sheet); storage != nullptr && *storage != nullptr) {
      **storage = std::move(text);
      if (document_ != nullptr) {
        document_->NoteTreeMutation();
      }
      return;
    }
    dom::Element* owner = SheetOwnerOf(sheet);
    if (owner == nullptr) {
      return;
    }
    if (owner->TagName() == "style") {
      ClearChildren(*owner, false);
      owner->Append(std::make_unique<dom::Text>(std::move(text)));
    } else {
      owner->SetLinkedStyleSheetText(std::move(text));
    }
    if (document_ != nullptr) {
      document_->NoteTreeMutation();
    }
  };

  const Value list_prototype = interpreter_->NewObjectValue();
  if (list_prototype.IsObject()) {
    interfaces_.object->Set("CSSRuleList", list_prototype);
    const Value length = interpreter_->NewNativeValue("length", [](NativeCall& call) {
      const Value* sheet_slot =
          call.self.IsObject() ? call.self.object->GetOwn(kCssomSheetSlot) : nullptr;
      if (sheet_slot == nullptr) {
        return Value::Number(0);
      }
      return Value::Number(static_cast<double>(css::ParseCssom(SheetText(*sheet_slot)).size()));
    });
    if (length.IsObject()) {
      length.object->Set(kOwnerSlot, OwnerValue(this));
      list_prototype.object->DefineAccessor("length", length.object, nullptr);
    }
    const Value item = interpreter_->NewNativeValue("item", [this, make_rule](NativeCall& call) {
      const Value* sheet_slot =
          call.self.IsObject() ? call.self.object->GetOwn(kCssomSheetSlot) : nullptr;
      if (sheet_slot == nullptr) {
        return Value::Null();
      }
      const std::vector<css::CssomRule> rules = css::ParseCssom(SheetText(*sheet_slot));
      const std::size_t index = static_cast<std::size_t>(js::ToNumber(Argument(call.arguments, 0)));
      if (index >= rules.size()) {
        return Value::Null();
      }
      return make_rule(rules[index]);
    });
    if (item.IsObject()) {
      item.object->Set(kOwnerSlot, OwnerValue(this));
      list_prototype.object->Set("item", item);
    }
  }

  const auto make_list = [this, list_prototype, make_rule](const Value& sheet) -> Value {
    if (sheet.IsObject()) {
      if (const Value* cached = sheet.object->GetOwn(kCssomRuleListSlot)) {
        return *cached;
      }
    }
    const Value target = interpreter_->NewObjectValue();
    if (!target.IsObject()) {
      return target;
    }
    if (list_prototype.IsObject()) {
      target.object->SetPrototype(list_prototype.object);
    }
    target.object->SetHidden(kCssomSheetSlot, sheet);

    const Value handler = interpreter_->NewObjectValue();
    if (!handler.IsObject()) {
      return target;
    }
    const auto trap = [this, &handler](const char* name, js::NativeFunction function) {
      const Value native = interpreter_->NewNativeValue(name, std::move(function));
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, OwnerValue(this));
        handler.object->Set(name, native);
      }
    };
    trap("get", [this, make_rule](NativeCall& call) -> Value {
      const Value proxy_target = Argument(call.arguments, 0);
      const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
      if (!key.IsSymbol()) {
        if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
          const Value* sheet_slot =
              proxy_target.IsObject() ? proxy_target.object->GetOwn(kCssomSheetSlot) : nullptr;
          if (sheet_slot == nullptr) {
            return Value::Undefined();
          }
          const std::vector<css::CssomRule> rules = css::ParseCssom(SheetText(*sheet_slot));
          return index < rules.size() ? make_rule(rules[index]) : Value::Undefined();
        }
      }
      return call.interpreter.GetPropertyValue(proxy_target, key);
    });
    js::Value* proxy_ctor = interpreter_->GlobalScope()->Lookup("Proxy");
    if (proxy_ctor == nullptr || !proxy_ctor->IsObject()) {
      return target;
    }
    const js::Result made =
        interpreter_->CallFunction(*proxy_ctor, Value::Undefined(), {target, handler});
    const Value list = made.IsAbrupt() ? target : made.value;
    if (sheet.IsObject()) {
      sheet.object->SetHidden(kCssomRuleListSlot, list);
    }
    return list;
  };

  const Value css_rules = interpreter_->NewNativeValue("cssRules", [this, make_list](NativeCall& call) {
    return make_list(call.self);
  });
  if (css_rules.IsObject()) {
    css_rules.object->Set(kOwnerSlot, OwnerValue(this));
    sheet_proto->object->DefineAccessor("cssRules", css_rules.object, nullptr);
  }

  const Value insert_rule = interpreter_->NewNativeValue(
      "insertRule", [this, set_sheet_text](NativeCall& call) -> Value {
        const std::string rule_text = js::ToString(Argument(call.arguments, 0));
        const std::vector<css::CssomRule> parsed = css::ParseCssom(rule_text);
        if (parsed.size() != 1) {
          return ThrowDom(call, "SyntaxError", "the given rule could not be parsed");
        }
        std::vector<css::CssomRule> rules = css::ParseCssom(SheetText(call.self));
        std::size_t index = 0;
        if (call.arguments.size() >= 2) {
          const double n = js::ToNumber(Argument(call.arguments, 1));
          if (n < 0.0 || n > static_cast<double>(rules.size())) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          index = static_cast<std::size_t>(n);
        }
        rules.insert(rules.begin() + static_cast<std::ptrdiff_t>(index), parsed[0]);
        set_sheet_text(call.self, JoinCssom(rules));
        return Value::Number(static_cast<double>(index));
      });
  if (insert_rule.IsObject()) {
    insert_rule.object->Set(kOwnerSlot, OwnerValue(this));
    sheet_proto->object->Set("insertRule", insert_rule);
  }

  const Value delete_rule = interpreter_->NewNativeValue(
      "deleteRule", [this, set_sheet_text](NativeCall& call) -> Value {
        std::vector<css::CssomRule> rules = css::ParseCssom(SheetText(call.self));
        const double n = js::ToNumber(Argument(call.arguments, 0));
        if (n < 0.0 || n >= static_cast<double>(rules.size())) {
          return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
        }
        rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(n));
        set_sheet_text(call.self, JoinCssom(rules));
        return Value::Undefined();
      });
  if (delete_rule.IsObject()) {
    delete_rule.object->Set(kOwnerSlot, OwnerValue(this));
    sheet_proto->object->Set("deleteRule", delete_rule);
  }
}

}  // namespace microbrowser::bindings
