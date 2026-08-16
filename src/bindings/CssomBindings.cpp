// `cssRules`, `insertRule`, `deleteRule`, the CSSRule type hierarchy, and
// `CSSStyleRule.selectorText` / `.style`.
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
#include "bindings/CssomInternals.h"
#include "bindings/WebIdl.h"
#include "css/Cssom.h"
#include "css/DeclarationText.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

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

bool IsStyleMethodName(const std::string& name) {
  return name == "cssText" || name == "setProperty" || name == "removeProperty" ||
         name == "getPropertyValue" || name == "getPropertyPriority" || name == "length" ||
         name == "item" || name == "parentRule" || name == "cssFloat";
}

const char* InterfaceFor(css::CssomRuleType type) {
  switch (type) {
    case css::CssomRuleType::Style:
      return "CSSStyleRule";
    case css::CssomRuleType::Media:
      return "CSSMediaRule";
    case css::CssomRuleType::Supports:
      return "CSSSupportsRule";
    case css::CssomRuleType::Import:
      return "CSSImportRule";
    case css::CssomRuleType::FontFace:
      return "CSSFontFaceRule";
    case css::CssomRuleType::Page:
      return "CSSPageRule";
    case css::CssomRuleType::Margin:
      return "CSSMarginRule";
    case css::CssomRuleType::Namespace:
      return "CSSNamespaceRule";
    case css::CssomRuleType::Keyframes:
      return "CSSKeyframesRule";
    case css::CssomRuleType::Unknown:
      return "CSSRule";
  }
  return "CSSRule";
}

}  // namespace

void DomBindings::InstallCssomSheetRules() {
  EnsureInterfaces();
  if (interpreter_ == nullptr || !interfaces_.IsObject()) {
    return;
  }
  const Value* sheet_proto = interfaces_.object->GetOwn("CSSStyleSheet");
  if (sheet_proto == nullptr || !sheet_proto->IsObject()) {
    return;
  }

  const Value css_rule = MakeInterface("CSSRule", Value::Undefined());
  const Value css_grouping = MakeInterface("CSSGroupingRule", css_rule);
  const Value css_condition = MakeInterface("CSSConditionRule", css_grouping);
  MakeInterface("CSSMediaRule", css_condition);
  MakeInterface("CSSSupportsRule", css_condition);
  const Value css_style_rule = MakeInterface("CSSStyleRule", css_grouping);
  MakeInterface("CSSImportRule", css_rule);
  MakeInterface("CSSFontFaceRule", css_rule);
  MakeInterface("CSSPageRule", css_grouping);
  MakeInterface("CSSNamespaceRule", css_rule);
  MakeInterface("CSSKeyframesRule", css_rule);
  MakeInterface("CSSMarginRule", css_rule);
  const Value css_style_declaration = MakeInterface("CSSStyleDeclaration", Value::Undefined());
  MakeInterface("CSSStyleProperties", css_style_declaration);
  MakeInterface("CSSPageDescriptors", css_style_declaration);
  MakeInterface("MediaList", Value::Undefined());

  if (Value* ctor = interpreter_->GlobalScope()->Lookup("CSSRule");
      ctor != nullptr && ctor->IsObject() && css_rule.IsObject()) {
    static constexpr struct {
      const char* name;
      double value;
    } kConstants[] = {
        {"STYLE_RULE", 1},     {"CHARSET_RULE", 2}, {"IMPORT_RULE", 3},    {"MEDIA_RULE", 4},
        {"FONT_FACE_RULE", 5}, {"PAGE_RULE", 6},    {"KEYFRAMES_RULE", 7}, {"KEYFRAME_RULE", 8},
        {"MARGIN_RULE", 9},    {"NAMESPACE_RULE", 10}, {"SUPPORTS_RULE", 12},
    };
    for (const auto& constant : kConstants) {
      js::Object::Property property;
      property.value = Value::Number(constant.value);
      property.writable = false;
      property.enumerable = true;
      property.configurable = false;
      ctor->object->Define(constant.name, js::Object::Property(property));
      css_rule.object->Define(constant.name, std::move(property));
    }
  }

  const auto write_sheet = [this](const Value& sheet, std::string text) {
    if (CssomSheetStorage* storage = CssomSheetStoragePtr(sheet); storage != nullptr && *storage != nullptr) {
      **storage = std::move(text);
      if (document_ != nullptr) {
        document_->NoteTreeMutation();
      }
      return;
    }
    dom::Element* owner = CssomSheetOwnerOf(sheet);
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

  const auto make_rule = [this](const Value& sheet, std::size_t index,
                                const css::CssomRule& parsed, const Value& parent) -> Value {
    const Value rule = interpreter_->NewObjectValue();
    if (!rule.IsObject()) {
      return rule;
    }
    if (const Value* proto = interfaces_.object->GetOwn(InterfaceFor(parsed.type));
        proto != nullptr && proto->IsObject()) {
      rule.object->SetPrototype(proto->object);
    }
    rule.object->SetHidden(kCssomSheetSlot, sheet);
    rule.object->SetHidden(kCssomIndexSlot, Value::Number(static_cast<double>(index)));
    if (parent.IsObject()) {
      rule.object->SetHidden(kCssomParentSlot, parent);
    }
    rule.object->Set(kOwnerSlot, OwnerValue(this));
    return rule;
  };

  const auto wrappers_of = [this](const Value& sheet) -> Value {
    if (sheet.IsObject()) {
      if (const Value* cached = sheet.object->GetOwn(kCssomRuleWrappersSlot);
          cached != nullptr && cached->IsObject() &&
          cached->object->GetKind() == js::Object::Kind::Array) {
        return *cached;
      }
    }
    const Value array = interpreter_->NewArrayValue({});
    if (sheet.IsObject() && array.IsObject()) {
      sheet.object->SetHidden(kCssomRuleWrappersSlot, array);
    }
    return array;
  };

  const auto sheet_of = [](const Value& container) -> Value {
    const Value* slot = CssomSheetSlotOf(container);
    return slot != nullptr ? *slot : container;
  };
  const auto is_rule = [](const Value& container) {
    js::Object* host = CssomHostObject(container);
    return host != nullptr && host->GetOwn(kCssomIndexSlot) != nullptr;
  };
  const auto children_of = [is_rule](const Value& container) {
    if (is_rule(container)) {
      const Value* sheet = CssomSheetSlotOf(container);
      if (sheet == nullptr) {
        return std::vector<css::CssomRule>{};
      }
      std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      const css::CssomRule* rule = LocateCssomRule(rules, container);
      return rule == nullptr ? std::vector<css::CssomRule>{} : rule->children;
    }
    return css::ParseCssom(CssomSheetText(container));
  };
  const auto parent_arg = [is_rule](const Value& container) {
    return is_rule(container) ? container : Value::Undefined();
  };

  const auto sync_wrappers = [this, make_rule, wrappers_of, sheet_of, children_of,
                              parent_arg](const Value& container) {
    const std::vector<css::CssomRule> rules = children_of(container);
    const Value existing = wrappers_of(container);
    if (existing.IsObject() && existing.object->GetKind() == js::Object::Kind::Array &&
        existing.object->ElementCount() == rules.size()) {
      for (std::size_t i = 0; i < rules.size(); ++i) {
        const Value rule = existing.object->GetElement(i);
        if (rule.IsObject()) {
          rule.object->SetHidden(kCssomIndexSlot, Value::Number(static_cast<double>(i)));
        }
      }
      return existing;
    }
    const Value sheet = sheet_of(container);
    const Value parent = parent_arg(container);
    std::vector<Value> made;
    made.reserve(rules.size());
    for (std::size_t i = 0; i < rules.size(); ++i) {
      made.push_back(make_rule(sheet, i, rules[i], parent));
    }
    const Value array = interpreter_->NewArrayValue(std::move(made));
    if (container.IsObject() && array.IsObject()) {
      container.object->SetHidden(kCssomRuleWrappersSlot, array);
    }
    return array;
  };

  const auto splice_wrapper = [make_rule, wrappers_of, sheet_of, parent_arg](
                                  const Value& container, std::size_t index,
                                  const css::CssomRule* inserted) {
    const Value array = wrappers_of(container);
    if (!array.IsObject() || array.object->GetKind() != js::Object::Kind::Array) {
      return;
    }
    const std::size_t count = array.object->ElementCount();
    if (inserted != nullptr) {
      array.object->ResizeElements(count + 1);
      for (std::size_t i = count; i > index; --i) {
        array.object->SetElement(i, array.object->GetElement(i - 1));
      }
      array.object->SetElement(index, make_rule(sheet_of(container), index, *inserted,
                                                parent_arg(container)));
    } else if (index < count) {
      for (std::size_t i = index + 1; i < count; ++i) {
        array.object->SetElement(i - 1, array.object->GetElement(i));
      }
      array.object->ResizeElements(count - 1);
    }
    for (std::size_t i = 0; i < array.object->ElementCount(); ++i) {
      const Value rule = array.object->GetElement(i);
      if (rule.IsObject()) {
        rule.object->SetHidden(kCssomIndexSlot, Value::Number(static_cast<double>(i)));
      }
    }
  };

  const auto rule_accessor = [this](const Value& proto, const char* name, js::NativeFunction getter,
                                    js::NativeFunction setter = nullptr) {
    if (!proto.IsObject()) {
      return;
    }
    const std::string get_name = std::string("get ") + name;
    const Value get = interpreter_->NewNativeValue(
        get_name.c_str(), [getter = std::move(getter)](NativeCall& call) -> Value {
          if (!IsCssomRuleThis(call.self)) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          return getter(call);
        });
    if (!get.IsObject()) {
      return;
    }
    get.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(get, 0);
    js::Object* set = nullptr;
    if (setter) {
      const std::string set_name = std::string("set ") + name;
      const Value native = interpreter_->NewNativeValue(
          set_name.c_str(), [setter = std::move(setter)](NativeCall& call) -> Value {
            if (!IsCssomRuleThis(call.self)) {
              return call.Throw("TypeError", "Illegal invocation");
            }
            return setter(call);
          });
      if (native.IsObject()) {
        native.object->Set(kOwnerSlot, OwnerValue(this));
        SetFunctionLength(native, 1);
        set = native.object;
      }
    }
    proto.object->DefineAccessor(name, get.object, set);
  };

  rule_accessor(css_rule, "type", [](NativeCall& call) -> Value {
    const Value* sheet = CssomSheetSlotOf(call.self);
    if (sheet == nullptr) {
      return Value::Number(0);
    }
    const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    const css::CssomRule* rule = LocateCssomRule(rules, call.self);
    return Value::Number(rule == nullptr ? 0 : static_cast<double>(rule->type));
  });
  rule_accessor(css_rule, "cssText", [](NativeCall& call) -> Value {
    const Value* sheet = CssomSheetSlotOf(call.self);
    if (sheet == nullptr) {
      return Value::String("");
    }
    const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    const css::CssomRule* rule = LocateCssomRule(rules, call.self);
    return Value::String(rule == nullptr ? "" : rule->css_text);
  }, [write_sheet](NativeCall& call) -> Value {
    const Value* sheet = CssomSheetSlotOf(call.self);
    if (sheet == nullptr) {
      return Value::Undefined();
    }
    std::vector<css::CssomRule> replacement =
        css::ParseCssom(js::ToString(Argument(call.arguments, 0)));
    if (replacement.size() != 1) {
      return Value::Undefined();
    }
    std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    css::CssomRule* rule = LocateCssomRule(rules, call.self);
    if (rule == nullptr) {
      return Value::Undefined();
    }
    *rule = std::move(replacement[0]);
    for (css::CssomRule& top : rules) {
      css::RefreshCssomCssText(top);
    }
    write_sheet(*sheet, css::JoinCssomRules(rules));
    return Value::Undefined();
  });
  rule_accessor(css_rule, "parentRule", [](NativeCall& call) {
    js::Object* host = CssomHostObject(call.self);
    if (host == nullptr) {
      return Value::Null();
    }
    const Value* parent = host->GetOwn(kCssomParentSlot);
    return parent != nullptr && parent->IsObject() ? *parent : Value::Null();
  });
  rule_accessor(css_rule, "parentStyleSheet", [](NativeCall& call) {
    const Value* sheet = CssomSheetSlotOf(call.self);
    return sheet == nullptr ? Value::Null() : *sheet;
  });

  const auto make_style = [this, write_sheet](const Value& rule) -> Value {
    if (rule.IsObject()) {
      if (const Value* cached = rule.object->GetOwn(kCssomStyleSlot)) {
        return *cached;
      }
    }
    const Value target = interpreter_->NewObjectValue();
    if (!target.IsObject()) {
      return target;
    }
    target.object->SetHidden(kCssomCssTextSlot, rule);
    const char* proto_name = "CSSStyleProperties";
    if (const Value* sheet = CssomSheetSlotOf(rule)) {
      const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      if (const css::CssomRule* parsed = LocateCssomRule(rules, rule);
          parsed != nullptr && parsed->type == css::CssomRuleType::Page) {
        proto_name = "CSSPageDescriptors";
        target.object->SetHidden(kCssomPageStyleSlot, Value::Bool(true));
      }
    }
    if (const Value* proto = interfaces_.object->GetOwn(proto_name);
        proto != nullptr && proto->IsObject()) {
      target.object->SetPrototype(proto->object);
    }

    const auto declarations_of = [](const Value& style) {
      const Value* sheet = CssomSheetSlotOf(style);
      std::vector<css::CssomRule> rules =
          sheet == nullptr ? std::vector<css::CssomRule>{} : css::ParseCssom(CssomSheetText(*sheet));
      css::CssomRule* parsed = LocateCssomRule(rules, style);
      return parsed == nullptr ? std::vector<css::Declaration>{} : parsed->declarations;
    };

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
    trap("get", [declarations_of](NativeCall& call) -> Value {
      const Value receiver_target = Argument(call.arguments, 0);
      const Value key = Argument(call.arguments, 1);
      if (key.IsSymbol()) {
        return call.interpreter.GetPropertyValue(receiver_target, KeyOfTrapArgument(key));
      }
      const std::string written = js::ToString(key);
      if (IsStyleMethodName(written)) {
        return call.interpreter.GetPropertyValue(receiver_target, KeyOfTrapArgument(key));
      }
      const std::vector<css::Declaration> declarations = declarations_of(receiver_target);
      const std::string wanted = ToCssName(written);
      for (const css::Declaration& declaration : declarations) {
        if (declaration.property == wanted) {
          return Value::String(declaration.value);
        }
      }
      const Value inherited =
          call.interpreter.GetPropertyValue(receiver_target, KeyOfTrapArgument(key));
      if (!inherited.IsUndefined()) {
        return inherited;
      }
      return Value::String("");
    });
    trap("set", [write_declarations, declarations_of](NativeCall& call) -> Value {
      const Value receiver_target = Argument(call.arguments, 0);
      const Value key = Argument(call.arguments, 1);
      if (key.IsSymbol()) {
        return Value::Bool(true);
      }
      const std::string written = js::ToString(key);
      if (IsStyleMethodName(written) && written != "cssText" && written != "cssFloat") {
        return Value::Bool(true);
      }
      if (written == "cssText" || written == "cssFloat") {
        const js::Result assigned = call.interpreter.SetProperty(
            receiver_target, KeyOfTrapArgument(key), Argument(call.arguments, 2));
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
      const std::string raw_value = js::ToString(Argument(call.arguments, 2));
      const std::string value(util::TrimAscii(raw_value));
      std::string property = ToCssName(written);
      std::string stored = value;
      std::string canonical;
      switch (css::CanonicaliseDeclaration(property, stored, &canonical)) {
        case css::DeclarationValidity::Invalid:
          return Value::Bool(true);
        case css::DeclarationValidity::Canonical:
          stored = std::move(canonical);
          break;
        case css::DeclarationValidity::Unknown:
          if (!CssomKeepsUnknownDeclaration(property, stored)) {
            return Value::Bool(true);
          }
          break;
      }
      std::vector<css::Declaration> declarations = declarations_of(receiver_target);
      bool replaced = false;
      for (css::Declaration& declaration : declarations) {
        if (declaration.property == property) {
          declaration.value = std::move(stored);
          replaced = true;
        }
      }
      if (!replaced && !stored.empty()) {
        css::Declaration added;
        added.property = std::move(property);
        added.value = std::move(stored);
        declarations.push_back(std::move(added));
      }
      write_declarations(receiver_target, std::move(declarations));
      return Value::Bool(true);
    });
    js::Value* proxy_ctor = interpreter_->GlobalScope()->Lookup("Proxy");
    if (proxy_ctor == nullptr || !proxy_ctor->IsObject()) {
      return target;
    }
    const js::Result made =
        interpreter_->CallFunction(*proxy_ctor, Value::Undefined(), {target, handler});
    const Value style = made.IsAbrupt() ? target : made.value;
    if (style.IsObject()) {
      if (const Value* proto = interfaces_.object->GetOwn(proto_name);
          proto != nullptr && proto->IsObject()) {
        style.object->SetPrototype(proto->object);
      }
      if (proto_name == std::string_view("CSSPageDescriptors")) {
        style.object->SetHidden(kCssomPageStyleSlot, Value::Bool(true));
      }
    }
    if (rule.IsObject()) {
      rule.object->SetHidden(kCssomStyleSlot, style);
    }
    return style;
  };

  InstallCssomStylePrototype(*interpreter_, interfaces_, OwnerValue(this), write_sheet, geometry_);

  rule_accessor(
      css_style_rule, "selectorText",
      [](NativeCall& call) -> Value {
        const Value* sheet = CssomSheetSlotOf(call.self);
        if (sheet == nullptr) {
          return Value::String("");
        }
        const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
        const css::CssomRule* rule = LocateCssomRule(rules, call.self);
        return Value::String(rule == nullptr ? "" : rule->prelude);
      },
      [write_sheet](NativeCall& call) -> Value {
        const Value* sheet = CssomSheetSlotOf(call.self);
        if (sheet == nullptr) {
          return Value::Undefined();
        }
        std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
        css::CssomRule* rule = LocateCssomRule(rules, call.self);
        if (rule == nullptr) {
          return Value::Undefined();
        }
        if (css::SetCssomSelectorText(*rule, js::ToString(Argument(call.arguments, 0)))) {
          write_sheet(*sheet, css::JoinCssomRules(rules));
        }
        return Value::Undefined();
      });
  rule_accessor(
      css_style_rule, "style",
      [make_style](NativeCall& call) { return make_style(call.self); },
      [make_style](NativeCall& call) -> Value {
        const Value style = make_style(call.self);
        if (!style.IsObject()) {
          return Value::Undefined();
        }
        const js::Result assigned = call.interpreter.SetProperty(
            style, js::PropertyKey("cssText"), Argument(call.arguments, 0));
        if (assigned.IsAbrupt()) {
          return call.ThrowValue(assigned.value);
        }
        return Argument(call.arguments, 0);
      });

  if (const Value* page = interfaces_.object->GetOwn("CSSPageRule");
      page != nullptr && page->IsObject()) {
    rule_accessor(*page, "selectorText",
                  [](NativeCall& call) -> Value {
                    const Value* sheet = CssomSheetSlotOf(call.self);
                    if (sheet == nullptr) {
                      return Value::String("");
                    }
                    const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
                    const css::CssomRule* rule = LocateCssomRule(rules, call.self);
                    return Value::String(rule == nullptr ? "" : rule->prelude);
                  },
                  [write_sheet](NativeCall& call) -> Value {
                    const Value* sheet = CssomSheetSlotOf(call.self);
                    if (sheet == nullptr) {
                      return Value::Undefined();
                    }
                    std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
                    css::CssomRule* rule = LocateCssomRule(rules, call.self);
                    if (rule == nullptr) {
                      return Value::Undefined();
                    }
                    if (css::SetCssomPageSelectorText(*rule, js::ToString(Argument(call.arguments, 0)))) {
                      write_sheet(*sheet, css::JoinCssomRules(rules));
                    }
                    return Value::Undefined();
                  });
    rule_accessor(*page, "style", [make_style](NativeCall& call) { return make_style(call.self); },
                  [make_style](NativeCall& call) -> Value {
                    const Value style = make_style(call.self);
                    if (!style.IsObject()) {
                      return Value::Undefined();
                    }
                    const js::Result assigned = call.interpreter.SetProperty(
                        style, js::PropertyKey("cssText"), Argument(call.arguments, 0));
                    if (assigned.IsAbrupt()) {
                      return call.ThrowValue(assigned.value);
                    }
                    return Argument(call.arguments, 0);
                  });
  }
  if (const Value* margin = interfaces_.object->GetOwn("CSSMarginRule");
      margin != nullptr && margin->IsObject()) {
    rule_accessor(*margin, "name", [](NativeCall& call) -> Value {
      const Value* sheet = CssomSheetSlotOf(call.self);
      if (sheet == nullptr) {
        return Value::String("");
      }
      const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      const css::CssomRule* rule = LocateCssomRule(rules, call.self);
      return Value::String(rule == nullptr ? "" : rule->at_name);
    });
    rule_accessor(*margin, "style", [make_style](NativeCall& call) { return make_style(call.self); },
                  [make_style](NativeCall& call) -> Value {
                    const Value style = make_style(call.self);
                    if (!style.IsObject()) {
                      return Value::Undefined();
                    }
                    const js::Result assigned = call.interpreter.SetProperty(
                        style, js::PropertyKey("cssText"), Argument(call.arguments, 0));
                    if (assigned.IsAbrupt()) {
                      return call.ThrowValue(assigned.value);
                    }
                    return Argument(call.arguments, 0);
                  });
  }

  rule_accessor(css_condition, "conditionText", [](NativeCall& call) -> Value {
    const Value* sheet = CssomSheetSlotOf(call.self);
    if (sheet == nullptr) {
      return Value::String("");
    }
    const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    const css::CssomRule* rule = LocateCssomRule(rules, call.self);
    return Value::String(rule == nullptr ? "" : rule->prelude);
  });

  const Value list_prototype = MakeInterface("CSSRuleList", Value::Undefined());
  if (list_prototype.IsObject()) {
    const Value length = interpreter_->NewNativeValue("get length", [children_of](NativeCall& call) {
      if (CssomSheetSlotOf(call.self) == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      const Value* container = CssomSheetSlotOf(call.self);
      return Value::Number(static_cast<double>(children_of(*container).size()));
    });
    if (length.IsObject()) {
      length.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(length, 0);
      list_prototype.object->DefineAccessor("length", length.object, nullptr);
    }
    const Value item =
        interpreter_->NewNativeValue("item", [this, sync_wrappers](NativeCall& call) {
          if (!RequireArguments(call, "CSSRuleList", "item", 1)) {
            return call.ThrownValue();
          }
          const Value* sheet_slot = CssomSheetSlotOf(call.self);
          if (sheet_slot == nullptr) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          const Value wrappers = sync_wrappers(*sheet_slot);
          const std::size_t index =
              static_cast<std::size_t>(js::ToNumber(Argument(call.arguments, 0)));
          if (!wrappers.IsObject() || index >= wrappers.object->ElementCount()) {
            return Value::Null();
          }
          return wrappers.object->GetElement(index);
        });
    if (item.IsObject()) {
      item.object->Set(kOwnerSlot, OwnerValue(this));
      item.object->Set("length", Value::Number(1));
      item.object->HideProperty("length");
      list_prototype.object->Set("item", item);
    }
  }

  const auto make_list = [this, list_prototype, sync_wrappers](const Value& sheet) -> Value {
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
    trap("get", [this, sync_wrappers](NativeCall& call) -> Value {
      const Value proxy_target = Argument(call.arguments, 0);
      const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
      if (!key.IsSymbol()) {
        if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
          const Value* sheet_slot = CssomSheetSlotOf(proxy_target);
          if (sheet_slot == nullptr) {
            return Value::Undefined();
          }
          const Value wrappers = sync_wrappers(*sheet_slot);
          if (!wrappers.IsObject() || index >= wrappers.object->ElementCount()) {
            return Value::Undefined();
          }
          return wrappers.object->GetElement(index);
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
    if (list.IsObject()) {
      list.object->SetHidden(kCssomSheetSlot, sheet);
      if (list_prototype.IsObject()) {
        list.object->SetPrototype(list_prototype.object);
      }
    }
    if (sheet.IsObject()) {
      sheet.object->SetHidden(kCssomRuleListSlot, list);
    }
    return list;
  };

  const Value css_rules =
      interpreter_->NewNativeValue("get cssRules", [this, make_list](NativeCall& call) {
        if (!IsCssomRuleThis(call.self) && !IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        return make_list(call.self);
      });
  const Value rules_getter =
      interpreter_->NewNativeValue("get rules", [this, make_list](NativeCall& call) {
        if (!IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        return make_list(call.self);
      });
  if (css_rules.IsObject()) {
    css_rules.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(css_rules, 0);
    sheet_proto->object->DefineAccessor("cssRules", css_rules.object, nullptr);
    if (css_grouping.IsObject()) {
      css_grouping.object->DefineAccessor("cssRules", css_rules.object, nullptr);
    }
  }
  if (rules_getter.IsObject()) {
    rules_getter.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(rules_getter, 0);
    sheet_proto->object->DefineAccessor("rules", rules_getter.object, nullptr);
  }

  const auto insert_at = [write_sheet, splice_wrapper, sync_wrappers, is_rule](
                             NativeCall& call, std::string rule_text, std::size_t index) -> Value {
    if (is_rule(call.self)) {
      const Value* sheet = CssomSheetSlotOf(call.self);
      if (sheet == nullptr) {
        return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
      }
      std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      css::CssomRule* parent = LocateCssomRule(rules, call.self);
      if (parent == nullptr || index > parent->children.size()) {
        return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
      }
      std::vector<css::CssomRule> parsed = css::ParseCssom(rule_text);
      if (parsed.size() != 1) {
        return ThrowDom(call, "SyntaxError", "the given rule could not be parsed");
      }
      if (parsed[0].type == css::CssomRuleType::Import ||
          parsed[0].type == css::CssomRuleType::Namespace) {
        return ThrowDom(call, "HierarchyRequestError",
                        "this rule cannot be inserted into a grouping rule");
      }
      sync_wrappers(call.self);
      parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(index),
                              parsed[0]);
      for (css::CssomRule& top : rules) {
        css::RefreshCssomCssText(top);
      }
      write_sheet(*sheet, css::JoinCssomRules(rules));
      splice_wrapper(call.self, index, &parsed[0]);
      return Value::Number(static_cast<double>(index));
    }
    std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(call.self));
    if (index > rules.size()) {
      return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
    }
    std::vector<css::CssomRule> parsed = css::ParseCssom(rule_text);
    if (parsed.size() != 1) {
      return ThrowDom(call, "SyntaxError", "the given rule could not be parsed");
    }
    sync_wrappers(call.self);
    rules.insert(rules.begin() + static_cast<std::ptrdiff_t>(index), parsed[0]);
    write_sheet(call.self, css::JoinCssomRules(rules));
    splice_wrapper(call.self, index, &parsed[0]);
    return Value::Number(static_cast<double>(index));
  };

  const Value insert_rule = interpreter_->NewNativeValue(
      "insertRule", [insert_at](NativeCall& call) -> Value {
        if (!IsCssomRuleThis(call.self) && !IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        if (!RequireArguments(call, "CSSStyleSheet", "insertRule", 1)) {
          return call.ThrownValue();
        }
        const std::string rule_text = js::ToString(Argument(call.arguments, 0));
        std::size_t index = 0;
        if (call.arguments.size() >= 2) {
          const double n = js::ToNumber(Argument(call.arguments, 1));
          if (n < 0.0) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          index = static_cast<std::size_t>(n);
        }
        return insert_at(call, rule_text, index);
      });
  if (insert_rule.IsObject()) {
    insert_rule.object->Set(kOwnerSlot, OwnerValue(this));
    insert_rule.object->Set("length", Value::Number(1));
    insert_rule.object->HideProperty("length");
    sheet_proto->object->Set("insertRule", insert_rule);
    if (css_grouping.IsObject()) {
      css_grouping.object->Set("insertRule", insert_rule);
    }
  }

  const Value delete_rule = interpreter_->NewNativeValue(
      "deleteRule", [write_sheet, splice_wrapper, sync_wrappers, is_rule](NativeCall& call) -> Value {
        if (!IsCssomRuleThis(call.self) && !IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        if (!RequireArguments(call, "CSSStyleSheet", "deleteRule", 1)) {
          return call.ThrownValue();
        }
        if (is_rule(call.self)) {
          const Value* sheet = CssomSheetSlotOf(call.self);
          if (sheet == nullptr) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
          css::CssomRule* parent = LocateCssomRule(rules, call.self);
          const double n = js::ToNumber(Argument(call.arguments, 0));
          if (parent == nullptr || n < 0.0 ||
              n >= static_cast<double>(parent->children.size())) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          const std::size_t index = static_cast<std::size_t>(n);
          sync_wrappers(call.self);
          parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
          for (css::CssomRule& top : rules) {
            css::RefreshCssomCssText(top);
          }
          write_sheet(*sheet, css::JoinCssomRules(rules));
          splice_wrapper(call.self, index, nullptr);
          return Value::Undefined();
        }
        std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(call.self));
        const double n = js::ToNumber(Argument(call.arguments, 0));
        if (n < 0.0 || n >= static_cast<double>(rules.size())) {
          return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
        }
        const std::size_t index = static_cast<std::size_t>(n);
        sync_wrappers(call.self);
        rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(index));
        write_sheet(call.self, css::JoinCssomRules(rules));
        splice_wrapper(call.self, index, nullptr);
        return Value::Undefined();
      });
  if (delete_rule.IsObject()) {
    delete_rule.object->Set(kOwnerSlot, OwnerValue(this));
    delete_rule.object->Set("length", Value::Number(1));
    delete_rule.object->HideProperty("length");
    sheet_proto->object->Set("deleteRule", delete_rule);
    if (css_grouping.IsObject()) {
      css_grouping.object->Set("deleteRule", delete_rule);
    }
  }

  const Value add_rule = interpreter_->NewNativeValue(
      "addRule", [insert_at](NativeCall& call) -> Value {
        if (!IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const std::string selector = call.arguments.empty()
                                         ? std::string("undefined")
                                         : js::ToString(Argument(call.arguments, 0));
        const std::string style = call.arguments.size() < 2
                                      ? std::string("undefined")
                                      : js::ToString(Argument(call.arguments, 1));
        std::string rule_text = selector;
        rule_text += " { ";
        rule_text += style;
        rule_text += " }";
        const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(call.self));
        std::size_t index = rules.size();
        if (call.arguments.size() >= 3) {
          const double n = js::ToNumber(Argument(call.arguments, 2));
          if (n < 0.0) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          index = static_cast<std::size_t>(n);
        }
        const Value result = insert_at(call, rule_text, index);
        if (call.HasThrown()) {
          return call.ThrownValue();
        }
        return result.IsNumber() ? Value::Number(-1) : result;
      });
  if (add_rule.IsObject()) {
    add_rule.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(add_rule, 0);
    sheet_proto->object->Set("addRule", add_rule);
  }

  const Value remove_rule = interpreter_->NewNativeValue(
      "removeRule", [write_sheet, splice_wrapper, sync_wrappers](NativeCall& call) -> Value {
        if (!IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(call.self));
        std::size_t index = 0;
        if (!call.arguments.empty()) {
          const double n = js::ToNumber(Argument(call.arguments, 0));
          if (n < 0.0 || n >= static_cast<double>(rules.size())) {
            return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
          }
          index = static_cast<std::size_t>(n);
        } else if (rules.empty()) {
          return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
        }
        if (index >= rules.size()) {
          return ThrowDom(call, "IndexSizeError", "the index is not a valid rule index");
        }
        sync_wrappers(call.self);
        rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(index));
        write_sheet(call.self, css::JoinCssomRules(rules));
        splice_wrapper(call.self, index, nullptr);
        return Value::Undefined();
      });
  if (remove_rule.IsObject()) {
    remove_rule.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(remove_rule, 0);
    sheet_proto->object->Set("removeRule", remove_rule);
  }

  for (const char* name :
       {"CSSRule", "CSSStyleRule", "CSSGroupingRule", "CSSConditionRule", "CSSMediaRule",
        "CSSSupportsRule", "CSSImportRule", "CSSFontFaceRule", "CSSPageRule", "CSSNamespaceRule",
        "CSSKeyframesRule", "CSSMarginRule", "CSSStyleDeclaration", "CSSStyleProperties",
        "CSSPageDescriptors", "CSSRuleList", "MediaList", "StyleSheet", "StyleSheetList"}) {
    if (js::Value* ctor = interpreter_->GlobalScope()->Lookup(name)) {
      SetFunctionLength(*ctor, 0);
      DefineNonEnumerable(interpreter_->Global(), name, *ctor);
    }
  }
  const auto inherit_ctor = [this](const char* child, const char* parent) {
    js::Value* child_ctor = interpreter_->GlobalScope()->Lookup(child);
    js::Value* parent_ctor = interpreter_->GlobalScope()->Lookup(parent);
    if (child_ctor != nullptr && child_ctor->IsObject() && parent_ctor != nullptr &&
        parent_ctor->IsObject()) {
      child_ctor->object->SetPrototype(parent_ctor->object);
    }
  };
  inherit_ctor("CSSGroupingRule", "CSSRule");
  inherit_ctor("CSSConditionRule", "CSSGroupingRule");
  inherit_ctor("CSSMediaRule", "CSSConditionRule");
  inherit_ctor("CSSSupportsRule", "CSSConditionRule");
  inherit_ctor("CSSStyleRule", "CSSGroupingRule");
  inherit_ctor("CSSPageRule", "CSSGroupingRule");
  inherit_ctor("CSSImportRule", "CSSRule");
  inherit_ctor("CSSFontFaceRule", "CSSRule");
  inherit_ctor("CSSNamespaceRule", "CSSRule");
  inherit_ctor("CSSKeyframesRule", "CSSRule");
  inherit_ctor("CSSMarginRule", "CSSRule");
  inherit_ctor("CSSStyleProperties", "CSSStyleDeclaration");
  inherit_ctor("CSSPageDescriptors", "CSSStyleDeclaration");
  inherit_ctor("CSSStyleSheet", "StyleSheet");
  InstallCssomMediaList(*interpreter_, interfaces_, OwnerValue(this), write_sheet);
}

}  // namespace microbrowser::bindings
