// MediaList on StyleSheet, CSSMediaRule, and CSSImportRule.

#include "bindings/CssomInternals.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/WebIdl.h"
#include "css/Cssom.h"

namespace microbrowser::bindings {
namespace {

using js::NativeCall;
using js::Value;

std::string TrimMedia(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::vector<std::string> SplitMediaList(std::string_view text) {
  std::vector<std::string> items;
  std::string current;
  int depth = 0;
  for (const char c : text) {
    if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    } else if (c == ',' && depth == 0) {
      std::string item = TrimMedia(current);
      if (!item.empty()) {
        items.push_back(std::move(item));
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  std::string item = TrimMedia(current);
  if (!item.empty()) {
    items.push_back(std::move(item));
  }
  return items;
}

std::string JoinMediaList(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) {
    if (!text.empty()) {
      text += ", ";
    }
    text += item;
  }
  return text;
}

const Value* MediaOwnerRule(const Value& list) {
  js::Object* host = CssomHostObject(list);
  if (host == nullptr) {
    return nullptr;
  }
  const Value* rule = host->GetOwn(kCssomParentSlot);
  return rule != nullptr && rule->IsObject() ? rule : nullptr;
}

std::string MediaTextOf(const Value& list) {
  const Value* sheet = CssomSheetSlotOf(list);
  if (sheet == nullptr) {
    return {};
  }
  if (const Value* rule = MediaOwnerRule(list)) {
    const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
    const css::CssomRule* parsed = LocateCssomRule(rules, *rule);
    return parsed == nullptr ? std::string() : TrimMedia(parsed->prelude);
  }
  const Value* sheet_value = sheet;
  const dom::Element* owner = CssomSheetOwnerOf(*sheet_value);
  if (owner == nullptr) {
    return {};
  }
  const std::string* media = owner->GetAttribute("media");
  return media == nullptr ? std::string() : *media;
}

std::string CssomQuoted(std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '"' || text[i] == '\'') {
      const char quote = text[i];
      std::size_t end = i + 1;
      while (end < text.size() && text[end] != quote) {
        ++end;
      }
      return std::string(text.substr(i + 1, end - i - 1));
    }
  }
  constexpr std::string_view kUrl = "url(";
  const auto pos = text.find(kUrl);
  if (pos != std::string_view::npos) {
    std::size_t start = pos + kUrl.size();
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
      ++start;
    }
    if (start < text.size() && (text[start] == '"' || text[start] == '\'')) {
      return CssomQuoted(text.substr(start));
    }
    std::size_t end = start;
    while (end < text.size() && text[end] != ')') {
      ++end;
    }
    return std::string(text.substr(start, end - start));
  }
  return {};
}

std::string CssomIdent(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n')) {
    ++start;
  }
  std::size_t end = start;
  while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' &&
         text[end] != '"' && text[end] != '\'') {
    ++end;
  }
  const std::string ident(text.substr(start, end - start));
  if (ident.size() >= 4 && (ident[0] == 'u' || ident[0] == 'U') && ident[1] == 'r' &&
      ident[2] == 'l' && ident[3] == '(') {
    return {};
  }
  return ident;
}

}  // namespace

void InstallCssomMediaList(js::Interpreter& interpreter, const js::Value& interfaces,
                           const js::Value& owner,
                           std::function<void(const js::Value&, std::string)> write_sheet) {
  if (!interfaces.IsObject()) {
    return;
  }
  const Value* proto = interfaces.object->GetOwn("MediaList");
  if (proto == nullptr || !proto->IsObject()) {
    return;
  }
  const Value media_list = *proto;

  const auto write_media = [write_sheet](const Value& list, std::string text) {
    const Value* sheet = CssomSheetSlotOf(list);
    if (sheet == nullptr) {
      return;
    }
    if (const Value* rule = MediaOwnerRule(list)) {
      std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      css::CssomRule* parsed = LocateCssomRule(rules, *rule);
      if (parsed == nullptr) {
        return;
      }
      parsed->prelude = std::move(text);
      for (css::CssomRule& top : rules) {
        css::RefreshCssomCssText(top);
      }
      write_sheet(*sheet, css::JoinCssomRules(rules));
      return;
    }
    if (dom::Element* owner_el = CssomSheetOwnerOf(*sheet)) {
      if (text.empty()) {
        owner_el->RemoveAttribute("media");
      } else {
        owner_el->SetAttribute("media", std::move(text));
      }
    }
  };

  const auto accessor = [&interpreter, &owner, &media_list](const char* name,
                                                            js::NativeFunction getter,
                                                            js::NativeFunction setter = nullptr) {
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
    media_list.object->DefineAccessor(name, get.object, set);
  };

  accessor(
      "mediaText",
      [](NativeCall& call) -> Value {
        if (CssomSheetSlotOf(call.self) == nullptr) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        return Value::String(MediaTextOf(call.self));
      },
      [write_media](NativeCall& call) -> Value {
        if (CssomSheetSlotOf(call.self) == nullptr) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        std::string text;
        if (!Argument(call.arguments, 0).IsNull()) {
          text = js::ToString(Argument(call.arguments, 0));
        }
        write_media(call.self, std::move(text));
        return Value::Undefined();
      });
  accessor("length", [](NativeCall& call) -> Value {
    if (CssomSheetSlotOf(call.self) == nullptr) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    return Value::Number(static_cast<double>(SplitMediaList(MediaTextOf(call.self)).size()));
  });

  const Value item = interpreter.NewNativeValue("item", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "MediaList", "item", 1)) {
      return call.ThrownValue();
    }
    if (CssomSheetSlotOf(call.self) == nullptr) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    const std::vector<std::string> items = SplitMediaList(MediaTextOf(call.self));
    const double n = js::ToNumber(Argument(call.arguments, 0));
    if (n < 0.0 || n >= static_cast<double>(items.size())) {
      return Value::Null();
    }
    return Value::String(items[static_cast<std::size_t>(n)]);
  });
  if (item.IsObject()) {
    item.object->Set(kOwnerSlot, owner);
    SetFunctionLength(item, 1);
    media_list.object->Set("item", item);
  }

  const Value append = interpreter.NewNativeValue(
      "appendMedium", [write_media](NativeCall& call) -> Value {
        if (!RequireArguments(call, "MediaList", "appendMedium", 1)) {
          return call.ThrownValue();
        }
        if (CssomSheetSlotOf(call.self) == nullptr) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const std::string medium = TrimMedia(js::ToString(Argument(call.arguments, 0)));
        if (medium.empty()) {
          return Value::Undefined();
        }
        std::vector<std::string> items = SplitMediaList(MediaTextOf(call.self));
        items.erase(std::remove(items.begin(), items.end(), medium), items.end());
        items.push_back(medium);
        write_media(call.self, JoinMediaList(items));
        return Value::Undefined();
      });
  if (append.IsObject()) {
    append.object->Set(kOwnerSlot, owner);
    SetFunctionLength(append, 1);
    media_list.object->Set("appendMedium", append);
  }

  const Value drop = interpreter.NewNativeValue(
      "deleteMedium", [write_media](NativeCall& call) -> Value {
        if (!RequireArguments(call, "MediaList", "deleteMedium", 1)) {
          return call.ThrownValue();
        }
        if (CssomSheetSlotOf(call.self) == nullptr) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const std::string medium = TrimMedia(js::ToString(Argument(call.arguments, 0)));
        std::vector<std::string> items = SplitMediaList(MediaTextOf(call.self));
        const auto found = std::find(items.begin(), items.end(), medium);
        if (found == items.end()) {
          return ThrowDom(call, "NotFoundError", "the medium is not in the collection");
        }
        items.erase(found);
        write_media(call.self, JoinMediaList(items));
        return Value::Undefined();
      });
  if (drop.IsObject()) {
    drop.object->Set(kOwnerSlot, owner);
    SetFunctionLength(drop, 1);
    media_list.object->Set("deleteMedium", drop);
  }

  const Value to_string = interpreter.NewNativeValue("toString", [](NativeCall& call) -> Value {
    if (CssomSheetSlotOf(call.self) == nullptr) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    return Value::String(MediaTextOf(call.self));
  });
  if (to_string.IsObject()) {
    to_string.object->Set(kOwnerSlot, owner);
    SetFunctionLength(to_string, 0);
    media_list.object->Set("toString", to_string);
  }

  const auto make_list = [media_list](js::Interpreter& js, const Value& sheet,
                                      const Value& rule) -> Value {
    const Value container = rule.IsObject() ? rule : sheet;
    if (container.IsObject()) {
      if (const Value* cached = container.object->GetOwn(kCssomMediaListSlot)) {
        return *cached;
      }
    }
    const Value target = js.NewObjectValue();
    if (!target.IsObject()) {
      return target;
    }
    target.object->SetPrototype(media_list.object);
    target.object->SetHidden(kCssomSheetSlot, sheet);
    if (rule.IsObject()) {
      target.object->SetHidden(kCssomParentSlot, rule);
    }

    const Value handler = js.NewObjectValue();
    if (!handler.IsObject()) {
      return target;
    }
    const Value getter = js.NewNativeValue("get", [](NativeCall& call) -> Value {
      const Value list_target = Argument(call.arguments, 0);
      const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
      if (!key.IsSymbol()) {
        if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
          const std::vector<std::string> items = SplitMediaList(MediaTextOf(list_target));
          if (index < items.size()) {
            return Value::String(items[index]);
          }
          return Value::Undefined();
        }
      }
      return call.interpreter.GetPropertyValue(list_target, key);
    });
    if (getter.IsObject()) {
      handler.object->Set("get", getter);
    }
    js::Value* proxy_ctor = js.GlobalScope()->Lookup("Proxy");
    Value list = target;
    if (proxy_ctor != nullptr && proxy_ctor->IsObject()) {
      const js::Result made =
          js.CallFunction(*proxy_ctor, Value::Undefined(), {target, handler});
      if (!made.IsAbrupt() && made.value.IsObject()) {
        list = made.value;
        list.object->SetHidden(kCssomSheetSlot, sheet);
        if (rule.IsObject()) {
          list.object->SetHidden(kCssomParentSlot, rule);
        }
        list.object->SetPrototype(media_list.object);
      }
    }
    if (container.IsObject()) {
      container.object->SetHidden(kCssomMediaListSlot, list);
    }
    return list;
  };

  const auto media_accessor = [&owner, &make_list](js::Interpreter& js, const Value& target_proto,
                                                   bool on_rule) {
    if (!target_proto.IsObject()) {
      return;
    }
    const Value get = js.NewNativeValue(
        "get media", [make_list, on_rule](NativeCall& call) -> Value {
          if (on_rule) {
            if (!IsCssomRuleThis(call.self)) {
              return call.Throw("TypeError", "Illegal invocation");
            }
            const Value* sheet = CssomSheetSlotOf(call.self);
            if (sheet == nullptr) {
              return Value::Null();
            }
            return make_list(call.interpreter, *sheet, call.self);
          }
          if (!IsCssomSheetThis(call.self)) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          return make_list(call.interpreter, call.self, Value::Undefined());
        });
        const Value set = js.NewNativeValue(
        "set media", [make_list, on_rule](NativeCall& call) -> Value {
          if ((on_rule && !IsCssomRuleThis(call.self)) ||
              (!on_rule && !IsCssomSheetThis(call.self))) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          Value sheet = call.self;
          if (on_rule) {
            const Value* slot = CssomSheetSlotOf(call.self);
            if (slot == nullptr) {
              return Value::Undefined();
            }
            sheet = *slot;
          }
          const Value list = make_list(call.interpreter, sheet,
                                       on_rule ? call.self : Value::Undefined());
          const js::Result assigned = call.interpreter.SetProperty(
              list, js::PropertyKey("mediaText"), Argument(call.arguments, 0));
          if (assigned.IsAbrupt()) {
            return call.ThrowValue(assigned.value);
          }
          return Value::Undefined();
        });
    if (get.IsObject()) {
      get.object->Set(kOwnerSlot, owner);
      SetFunctionLength(get, 0);
      js::Object* setter = nullptr;
      if (set.IsObject()) {
        set.object->Set(kOwnerSlot, owner);
        SetFunctionLength(set, 1);
        setter = set.object;
      }
      target_proto.object->DefineAccessor("media", get.object, setter);
    }
  };

  if (const Value* sheet = interfaces.object->GetOwn("StyleSheet")) {
    media_accessor(interpreter, *sheet, false);
  }
  if (const Value* media_rule = interfaces.object->GetOwn("CSSMediaRule")) {
    media_accessor(interpreter, *media_rule, true);
  }
  if (const Value* import_rule = interfaces.object->GetOwn("CSSImportRule")) {
    media_accessor(interpreter, *import_rule, true);
  }

  const auto rule_get = [&interpreter, &owner](const Value& target, const char* name,
                                               js::NativeFunction getter) {
    if (!target.IsObject()) {
      return;
    }
    const std::string get_name = std::string("get ") + name;
    const Value get = interpreter.NewNativeValue(
        get_name.c_str(), [getter = std::move(getter)](NativeCall& call) -> Value {
          if (!IsCssomRuleThis(call.self)) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          return getter(call);
        });
    if (!get.IsObject()) {
      return;
    }
    get.object->Set(kOwnerSlot, owner);
    SetFunctionLength(get, 0);
    target.object->DefineAccessor(name, get.object, nullptr);
  };
  if (const Value* import_rule = interfaces.object->GetOwn("CSSImportRule");
      import_rule != nullptr && import_rule->IsObject()) {
    rule_get(*import_rule, "href", [](NativeCall& call) -> Value {
      const Value* sheet = CssomSheetSlotOf(call.self);
      if (sheet == nullptr) {
        return Value::String("");
      }
      const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      const css::CssomRule* rule = LocateCssomRule(rules, call.self);
      return Value::String(rule == nullptr ? "" : CssomQuoted(rule->prelude));
    });
    rule_get(*import_rule, "layerName", [](NativeCall&) { return Value::String(""); });
    rule_get(*import_rule, "supportsText", [](NativeCall&) { return Value::String(""); });
    rule_get(*import_rule, "styleSheet", [](NativeCall&) { return Value::Null(); });
  }
  if (const Value* ns_rule = interfaces.object->GetOwn("CSSNamespaceRule");
      ns_rule != nullptr && ns_rule->IsObject()) {
    rule_get(*ns_rule, "namespaceURI", [](NativeCall& call) -> Value {
      const Value* sheet = CssomSheetSlotOf(call.self);
      if (sheet == nullptr) {
        return Value::String("");
      }
      const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      const css::CssomRule* rule = LocateCssomRule(rules, call.self);
      return Value::String(rule == nullptr ? "" : CssomQuoted(rule->prelude));
    });
    rule_get(*ns_rule, "prefix", [](NativeCall& call) -> Value {
      const Value* sheet = CssomSheetSlotOf(call.self);
      if (sheet == nullptr) {
        return Value::String("");
      }
      const std::vector<css::CssomRule> rules = css::ParseCssom(CssomSheetText(*sheet));
      const css::CssomRule* rule = LocateCssomRule(rules, call.self);
      if (rule == nullptr) {
        return Value::String("");
      }
      const std::string prefix = CssomIdent(rule->prelude);
      return Value::String(prefix);
    });
  }
}

}  // namespace microbrowser::bindings
