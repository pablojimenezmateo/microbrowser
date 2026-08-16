// `CSSStyleSheet`, `replaceSync`, and `adoptedStyleSheets`.
//
// ADR 0019 §4: one parsed sheet shared by every component instance rather than
// a `<style>` per instance. Lit on reddit and youtube depends on native
// constructable stylesheets when `attachShadow` exists and skips the polyfill.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/CssomInternals.h"
#include "bindings/DomBindings.h"
#include "bindings/WebIdl.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "url/Url.h"
#include "util/PerformanceCounters.h"
#include "util/StringUtil.h"

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

dom::SharedConstructableSheet SharedSheetText(const Value& sheet) {
  const SheetStorage* storage = SheetStoragePtr(sheet);
  if (storage == nullptr || *storage == nullptr) {
    return nullptr;
  }
  return *storage;
}

dom::Node* AdoptedStyleRootOf(dom::Node* node) {
  if (node == nullptr) {
    return nullptr;
  }
  if (node->GetKind() == dom::Node::Kind::Document) {
    return node;
  }
  if (node->IsDocumentFragment()) {
    auto& fragment = static_cast<dom::DocumentFragment&>(*node);
    return fragment.Host() == nullptr ? nullptr : node;
  }
  return nullptr;
}

void SetAdoptedStyleSheetsOn(dom::Node& root, std::vector<dom::SharedConstructableSheet> sheets) {
  if (root.GetKind() == dom::Node::Kind::Document) {
    static_cast<dom::Document&>(root).SetAdoptedStyleSheets(std::move(sheets));
    return;
  }
  if (root.IsDocumentFragment()) {
    static_cast<dom::DocumentFragment&>(root).SetAdoptedStyleSheets(std::move(sheets));
  }
}

const std::vector<dom::SharedConstructableSheet>& AdoptedStyleSheetsOf(const dom::Node& root) {
  if (root.GetKind() == dom::Node::Kind::Document) {
    return static_cast<const dom::Document&>(root).AdoptedStyleSheets();
  }
  return static_cast<const dom::DocumentFragment&>(root).AdoptedStyleSheets();
}

bool RelHasStylesheet(const dom::Element& element) {
  const std::string* rel = element.GetAttribute("rel");
  if (rel == nullptr) {
    return false;
  }
  std::size_t i = 0;
  while (i < rel->size()) {
    while (i < rel->size() && ((*rel)[i] == ' ' || (*rel)[i] == '\t' || (*rel)[i] == '\n' ||
                               (*rel)[i] == '\r' || (*rel)[i] == '\f')) {
      ++i;
    }
    const std::size_t start = i;
    while (i < rel->size() && (*rel)[i] != ' ' && (*rel)[i] != '\t' && (*rel)[i] != '\n' &&
           (*rel)[i] != '\r' && (*rel)[i] != '\f') {
      ++i;
    }
    if (i > start &&
        util::EqualsAsciiCaseInsensitive(std::string_view(rel->data() + start, i - start),
                                         "stylesheet")) {
      return true;
    }
  }
  return false;
}

bool IsAssociatedStyleSheetElement(const dom::Element& element) {
  if (element.TagName() == "style") {
    return true;
  }
  return element.TagName() == "link" && RelHasStylesheet(element);
}

bool IsInDocumentOrShadowTree(const dom::Node& node) {
  const dom::Node* current = &node;
  while (current != nullptr) {
    if (current->GetKind() == dom::Node::Kind::Document) {
      return true;
    }
    if (current->IsDocumentFragment() &&
        static_cast<const dom::DocumentFragment&>(*current).Host() != nullptr) {
      return true;
    }
    current = current->Parent();
  }
  return false;
}

std::vector<dom::Element*> AssociatedStyleSheetElements(dom::Node& root) {
  std::vector<dom::Element*> out;
  root.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    auto& element = const_cast<dom::Element&>(static_cast<const dom::Element&>(node));
    if (IsAssociatedStyleSheetElement(element)) {
      out.push_back(&element);
    }
  });
  return out;
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

}  // namespace

void DomBindings::InstallConstructableStylesheets(const js::Value& document_interface,
                                                  const js::Value& shadow_root_interface) {
  EnsureInterfaces();
  if (interpreter_ == nullptr) {
    return;
  }

  const Value prototype = MakeInterface("StyleSheet", Value::Undefined());
  if (!prototype.IsObject()) {
    return;
  }

  const auto accessor = [this, &prototype](const char* name, js::NativeFunction getter) {
    const std::string native_name = std::string("get ") + name;
    const Value native = interpreter_->NewNativeValue(
        native_name.c_str(), [getter = std::move(getter)](NativeCall& call) -> Value {
          if (!IsCssomSheetThis(call.self)) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          return getter(call);
        });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(native, 0);
      prototype.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  accessor("type", [](NativeCall&) { return Value::String("text/css"); });
  accessor("parentStyleSheet", [](NativeCall&) { return Value::Null(); });
  accessor("ownerNode", [this](NativeCall& call) {
    return WrapperFor(SheetOwnerOf(call.self));
  });
  accessor("href", [this](NativeCall& call) -> Value {
    const dom::Element* owner = SheetOwnerOf(call.self);
    if (owner == nullptr || owner->TagName() != "link") {
      return Value::Null();
    }
    const std::string* href = owner->GetAttribute("href");
    if (href == nullptr) {
      return Value::Null();
    }
    const std::optional<url::Url> base = url::Url::Parse(url_);
    const std::optional<url::Url> resolved =
        base.has_value() ? url::Url::Parse(*href, *base) : url::Url::Parse(*href);
    return Value::String(resolved.has_value() ? resolved->Serialize() : *href);
  });
  accessor("title", [](NativeCall& call) -> Value {
    const dom::Element* owner = SheetOwnerOf(call.self);
    if (owner == nullptr) {
      return Value::String("");
    }
    const std::string* title = owner->GetAttribute("title");
    return Value::String(title == nullptr ? "" : *title);
  });

  const Value disabled_get = interpreter_->NewNativeValue(
      "get disabled", [](NativeCall& call) -> Value {
        if (!IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        const dom::Element* owner = SheetOwnerOf(call.self);
        return Value::Bool(owner != nullptr && owner->GetAttribute("disabled") != nullptr);
      });
  const Value disabled_set = interpreter_->NewNativeValue(
      "set disabled", [](NativeCall& call) -> Value {
        if (!IsCssomSheetThis(call.self)) {
          return call.Throw("TypeError", "Illegal invocation");
        }
        dom::Element* owner = SheetOwnerOf(call.self);
        if (owner == nullptr) {
          return Value::Undefined();
        }
        if (js::ToBoolean(Argument(call.arguments, 0))) {
          owner->SetAttribute("disabled", "");
        } else {
          owner->RemoveAttribute("disabled");
        }
        return Value::Undefined();
      });
  if (disabled_get.IsObject() && disabled_set.IsObject()) {
    disabled_get.object->Set(kOwnerSlot, OwnerValue(this));
    disabled_set.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(disabled_get, 0);
    SetFunctionLength(disabled_set, 1);
    prototype.object->DefineAccessor("disabled", disabled_get.object, disabled_set.object);
  }

  const Value css_proto = MakeInterface("CSSStyleSheet", prototype);
  if (!css_proto.IsObject()) {
    return;
  }
  const auto css_accessor = [this, &css_proto](const char* name, js::NativeFunction getter) {
    const std::string native_name = std::string("get ") + name;
    const Value native = interpreter_->NewNativeValue(
        native_name.c_str(), [getter = std::move(getter)](NativeCall& call) -> Value {
          if (!IsCssomSheetThis(call.self)) {
            return call.Throw("TypeError", "Illegal invocation");
          }
          return getter(call);
        });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(native, 0);
      css_proto.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  css_accessor("ownerRule", [](NativeCall&) { return Value::Null(); });

  const Value replace_sync = interpreter_->NewNativeValue("replaceSync", [this](NativeCall& call) {
    if (!IsCssomSheetThis(call.self)) {
      return call.Throw("TypeError", "Illegal invocation");
    }
    if (!RequireArguments(call, "CSSStyleSheet", "replaceSync", 1)) {
      return call.ThrownValue();
    }
    SheetStorage* storage = SheetStoragePtr(call.self);
    if (storage == nullptr || *storage == nullptr) {
      return ThrowDom(call, "NotAllowedError",
                      "replaceSync is not allowed on this CSSStyleSheet");
    }
    **storage = js::ToString(Argument(call.arguments, 0));
    if (call.self.IsObject()) {
      call.self.object->SetHidden(kCssomRuleWrappersSlot, Value::Undefined());
    }
    if (document_ != nullptr) {
      document_->NoteTreeMutation();
    }
    return Value::Undefined();
  });
  if (replace_sync.IsObject()) {
    replace_sync.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(replace_sync, 1);
    css_proto.object->Set("replaceSync", replace_sync);
  }

  const Value replace = interpreter_->NewNativeValue("replace", [](NativeCall& call) {
    const Value promise = call.interpreter.NewPromiseValue();
    if (!promise.IsObject()) {
      return Value::Undefined();
    }
    if (!IsCssomSheetThis(call.self)) {
      call.interpreter.SettleAsyncResult(
          promise.object,
          call.interpreter.MakeError("TypeError", "Illegal invocation"), true);
      return promise;
    }
    if (call.arguments.empty()) {
      call.interpreter.SettleAsyncResult(
          promise.object,
          call.interpreter.MakeError("TypeError", "replace requires 1 argument"), true);
      return promise;
    }
    SheetStorage* storage = SheetStoragePtr(call.self);
    if (storage == nullptr || *storage == nullptr) {
      call.interpreter.SettleAsyncResult(
          promise.object,
          MakeDomException(call.interpreter, "NotAllowedError",
                           "replace is not allowed on this CSSStyleSheet"),
          true);
      return promise;
    }
    // The async form can `@import`; absent rather than a sheet that silently
    // lacks its imports. ADR 0019 §4.
    call.interpreter.SettleAsyncResult(
        promise.object,
        MakeDomException(call.interpreter, "NotSupportedError",
                         "CSSStyleSheet.replace is not supported"),
        true);
    return promise;
  });
  if (replace.IsObject()) {
    replace.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(replace, 1);
    css_proto.object->Set("replace", replace);
  }

  DomBindings* self = this;
  const Value constructor = interpreter_->NewNativeValue("CSSStyleSheet", [self](NativeCall& call) {
    if (!call.interpreter.IsConstructCall(call.self)) {
      return call.Throw("TypeError", "Illegal constructor: CSSStyleSheet");
    }
    const Value sheet = call.interpreter.NewObjectValue();
    if (!sheet.IsObject()) {
      return Value::Undefined();
    }
    if (const Value* style_sheet_proto = self->interfaces_.object->GetOwn("CSSStyleSheet")) {
      sheet.object->SetPrototype(style_sheet_proto->object);
    }
    // Owned by the binding layer, not released into the void. See DomBindings::sheet_texts_.
    SheetStorage* heap = &self->sheet_texts_.emplace_back(std::make_shared<std::string>());
    sheet.object->SetHidden(kCSSStyleSheetMarkerSlot, Value::Bool(true));
    sheet.object->SetHidden(kCSSSheetStorageSlot, PointerValue(heap));
    if (call.arguments.size() >= 1 && !Argument(call.arguments, 0).IsUndefined()) {
      **heap = js::ToString(Argument(call.arguments, 0));
    }
    return sheet;
  });
  if (constructor.IsObject()) {
    js::Object::Property proto_property;
    proto_property.value = css_proto;
    proto_property.writable = false;
    proto_property.enumerable = false;
    proto_property.configurable = false;
    constructor.object->Define("prototype", std::move(proto_property));
    css_proto.object->Set("constructor", constructor);
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    SetFunctionLength(constructor, 0);
    DefineNonEnumerable(interpreter_->Global(), "CSSStyleSheet", constructor);
    interpreter_->GlobalScope()->Declare("CSSStyleSheet", constructor, false);
    if (js::Value* style_sheet = interpreter_->GlobalScope()->Lookup("StyleSheet")) {
      interpreter_->Global()->Set("StyleSheet", *style_sheet);
    }
  }

  const auto install_adopted = [this](const js::Value& target) {
    if (!target.IsObject()) {
      return;
    }
    const Value getter = interpreter_->NewNativeValue("get adoptedStyleSheets", [this](NativeCall& call) {
      dom::Node* root = AdoptedStyleRootOf(NodeOf(call.self));
      if (root == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      std::vector<Value> out;
      for (const dom::SharedConstructableSheet& text : AdoptedStyleSheetsOf(*root)) {
        if (text == nullptr) {
          continue;
        }
        const Value sheet = interpreter_->NewObjectValue();
        if (!sheet.IsObject()) {
          continue;
        }
        if (const Value* style_sheet_proto = interfaces_.object->GetOwn("CSSStyleSheet")) {
          sheet.object->SetPrototype(style_sheet_proto->object);
        }
        SheetStorage* storage = &sheet_texts_.emplace_back(text);
        sheet.object->SetHidden(kCSSStyleSheetMarkerSlot, Value::Bool(true));
        sheet.object->SetHidden(kCSSSheetStorageSlot, PointerValue(storage));
        out.push_back(sheet);
      }
      return call.interpreter.NewArrayValue(std::move(out));
    });
    const Value setter = interpreter_->NewNativeValue("adoptedStyleSheets", [this](NativeCall& call) {
      dom::Node* root = AdoptedStyleRootOf(NodeOf(call.self));
      const Value list = Argument(call.arguments, 0);
      if (root == nullptr || !list.IsObject()) {
        return call.Throw("TypeError", "Failed to set adoptedStyleSheets");
      }
      std::vector<dom::SharedConstructableSheet> adopted;
      const std::size_t count = list.object->ElementCount();
      adopted.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        const Value entry = list.object->GetElement(i);
        SheetStorage shared = SharedSheetText(entry);
        if (shared == nullptr) {
          return call.Throw("TypeError",
                            "adoptedStyleSheets must be an array of CSSStyleSheet objects");
        }
        adopted.push_back(std::move(shared));
      }
      SetAdoptedStyleSheetsOn(*root, std::move(adopted));
      if (document_ != nullptr) {
        document_->NoteTreeMutation();
      }
      return Value::Undefined();
    });
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(getter, 0);
      SetFunctionLength(setter, 1);
      target.object->DefineAccessor("adoptedStyleSheets", getter.object, setter.object);
    }
  };

  install_adopted(document_interface);
  install_adopted(shadow_root_interface);

  // document.styleSheets / shadowRoot.styleSheets: the associated sheets of
  // `<style>` and `<link rel=stylesheet>` in tree order. Adopted sheets are
  // not in this list -- the tests that say so are why. cssRules is still
  // absent: a short rule list from flattened `@media` would be a wrong answer
  // (ADR 0012), and retaining at-rules is the next slice.
  const auto associated_sheet = [this](dom::Element& element) -> Value {
    const Value owner_wrapper = WrapperFor(&element);
    if (owner_wrapper.IsObject()) {
      if (const Value* cached = owner_wrapper.object->GetOwn(kAssociatedSheetSlot)) {
        return *cached;
      }
    }
    const Value sheet = interpreter_->NewObjectValue();
    if (!sheet.IsObject()) {
      return sheet;
    }
    if (const Value* style_sheet_proto = interfaces_.object->GetOwn("CSSStyleSheet")) {
      sheet.object->SetPrototype(style_sheet_proto->object);
    }
    sheet.object->SetHidden(kCSSStyleSheetMarkerSlot, Value::Bool(true));
    sheet.object->SetHidden(kSheetOwnerSlot, PointerValue(&element));
    if (owner_wrapper.IsObject()) {
      owner_wrapper.object->SetHidden(kAssociatedSheetSlot, sheet);
    }
    return sheet;
  };

  const Value list_prototype = MakeInterface("StyleSheetList", Value::Undefined());
  if (list_prototype.IsObject()) {
    const Value length = interpreter_->NewNativeValue("get length", [](NativeCall& call) -> Value {
      dom::Node* root = NodeOf(call.self);
      if (root == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      return Value::Number(static_cast<double>(AssociatedStyleSheetElements(*root).size()));
    });
    if (length.IsObject()) {
      length.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(length, 0);
      list_prototype.object->DefineAccessor("length", length.object, nullptr);
    }
    const Value item = interpreter_->NewNativeValue("item", [this, associated_sheet](NativeCall& call) {
      if (!RequireArguments(call, "StyleSheetList", "item", 1)) {
        return call.ThrownValue();
      }
      dom::Node* root = NodeOf(call.self);
      if (root == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      const std::vector<dom::Element*> sheets = AssociatedStyleSheetElements(*root);
      const std::size_t index = static_cast<std::size_t>(js::ToNumber(Argument(call.arguments, 0)));
      if (index >= sheets.size()) {
        return Value::Null();
      }
      return associated_sheet(*sheets[index]);
    });
    if (item.IsObject()) {
      item.object->Set(kOwnerSlot, OwnerValue(this));
      item.object->Set("length", Value::Number(1));
      item.object->HideProperty("length");
      list_prototype.object->Set("item", item);
    }
  }
  if (js::Value* list_ctor = interpreter_->GlobalScope()->Lookup("StyleSheetList")) {
    interpreter_->Global()->Set("StyleSheetList", *list_ctor);
  }

  const auto make_list = [this, list_prototype, associated_sheet](dom::Node& root) -> Value {
    const Value host = WrapperFor(&root);
    if (host.IsObject()) {
      if (const Value* cached = host.object->GetOwn(kStyleSheetListSlot)) {
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
    target.object->SetHidden(kNodeSlot, PointerValue(&root));

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
    trap("get", [this, associated_sheet](NativeCall& call) -> Value {
      const Value proxy_target = Argument(call.arguments, 0);
      const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
      if (!key.IsSymbol()) {
        if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
          dom::Node* tree = NodeOf(proxy_target);
          if (tree == nullptr) {
            return Value::Undefined();
          }
          const std::vector<dom::Element*> sheets = AssociatedStyleSheetElements(*tree);
          return index < sheets.size() ? associated_sheet(*sheets[index]) : Value::Undefined();
        }
      }
      return call.interpreter.GetPropertyValue(proxy_target, key);
    });
    trap("has", [](NativeCall& call) -> Value {
      const Value proxy_target = Argument(call.arguments, 0);
      const js::PropertyKey key = KeyOfTrapArgument(Argument(call.arguments, 1));
      if (!key.IsSymbol()) {
        if (const std::size_t index = ArrayIndexOf(key.Text()); index != kNotAnIndex) {
          dom::Node* tree = NodeOf(proxy_target);
          const std::size_t count =
              tree == nullptr ? 0 : AssociatedStyleSheetElements(*tree).size();
          return Value::Bool(index < count);
        }
      }
      return Value::Bool(proxy_target.IsObject() &&
                         proxy_target.object->GetProperty(key) != nullptr);
    });
    js::Value* proxy_ctor = interpreter_->GlobalScope()->Lookup("Proxy");
    if (proxy_ctor == nullptr || !proxy_ctor->IsObject()) {
      return target;
    }
    const js::Result made =
        interpreter_->CallFunction(*proxy_ctor, Value::Undefined(), {target, handler});
    const Value list = made.IsAbrupt() ? target : made.value;
    if (host.IsObject()) {
      host.object->SetHidden(kStyleSheetListSlot, list);
    }
    return list;
  };

  const auto install_style_sheets = [this, make_list](const Value& target) {
    if (!target.IsObject()) {
      return;
    }
    const Value getter = interpreter_->NewNativeValue("get styleSheets", [this, make_list](NativeCall& call) {
      dom::Node* root = NodeOf(call.self);
      if (root == nullptr) {
        return call.Throw("TypeError", "Illegal invocation");
      }
      return make_list(*root);
    });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      SetFunctionLength(getter, 0);
      target.object->DefineAccessor("styleSheets", getter.object, nullptr);
    }
  };
  install_style_sheets(document_interface);
  install_style_sheets(shadow_root_interface);

  const auto install_element_sheet = [this, associated_sheet](const char* interface) {
    const Value* proto = interfaces_.IsObject() ? interfaces_.object->GetOwn(interface) : nullptr;
    if (proto == nullptr || !proto->IsObject()) {
      return;
    }
    const Value getter = interpreter_->NewNativeValue("sheet", [this, associated_sheet](NativeCall& call) {
      dom::Node* node = NodeOf(call.self);
      if (node == nullptr || !node->IsElement()) {
        return Value::Null();
      }
      auto& element = static_cast<dom::Element&>(*node);
      if (!IsAssociatedStyleSheetElement(element) || !IsInDocumentOrShadowTree(element)) {
        return Value::Null();
      }
      return associated_sheet(element);
    });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      proto->object->DefineAccessor("sheet", getter.object, nullptr);
    }
  };
  install_element_sheet("HTMLStyleElement");
  install_element_sheet("HTMLLinkElement");
  InstallCssomSheetRules();
}

void DomBindings::InstallCssOm() {
  if (interpreter_ == nullptr) {
    return;
  }
  const Value css = interpreter_->NewObjectValue();
  if (!css.IsObject()) {
    return;
  }

  // Both call shapes go through the same answers `@supports` uses
  // (SupportsDeclaration / SupportsConditionText), so a page probing via
  // `CSS.supports` cannot disagree with a stylesheet's `@supports`.
  const Value supports = interpreter_->NewNativeValue("supports", [](NativeCall& call) -> Value {
    if (call.arguments.empty()) {
      return Value::Bool(false);
    }
    bool ok = false;
    if (call.arguments.size() >= 2) {
      ok = css::SupportsDeclaration(js::ToString(call.arguments[0]),
                                    js::ToString(call.arguments[1]));
    } else {
      ok = css::SupportsConditionText(js::ToString(call.arguments[0]));
    }
    util::AddPerformanceCounter(util::PerfCounterId::CssSupportsQueries);
    return Value::Bool(ok);
  });
  if (supports.IsObject()) {
    css.object->Set("supports", supports);
  }

  // CSS.escape: enough for an ident. A fuller escapable-string grammar is not
  // what pages probe for; they need a function that turns `a b` into something
  // `querySelector` can take.
  const Value escape = interpreter_->NewNativeValue("escape", [](NativeCall& call) -> Value {
    const std::string input = js::ToString(Argument(call.arguments, 0));
    std::string out;
    out.reserve(input.size() + 8);
    std::size_t unit = 0;
    for (std::size_t i = 0; i < input.size();) {
      const unsigned char lead = static_cast<unsigned char>(input[i]);
      std::uint32_t cp = 0;
      std::size_t width = 1;
      if (lead < 0x80) {
        cp = lead;
      } else if ((lead & 0xE0) == 0xC0 && i + 1 < input.size()) {
        cp = (lead & 0x1F) << 6 | (static_cast<unsigned char>(input[i + 1]) & 0x3F);
        width = 2;
      } else if ((lead & 0xF0) == 0xE0 && i + 2 < input.size()) {
        cp = static_cast<std::uint32_t>(lead & 0x0F) << 12 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3F) << 6 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3F);
        width = 3;
      } else if ((lead & 0xF8) == 0xF0 && i + 3 < input.size()) {
        cp = static_cast<std::uint32_t>(lead & 0x07) << 18 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1]) & 0x3F) << 12 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]) & 0x3F) << 6 |
             static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 3]) & 0x3F);
        width = 4;
      } else {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\%x ", lead);
        out += buf;
        ++i;
        ++unit;
        continue;
      }
      const bool as_name =
          (cp == '_' || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0x80) ||
          (unit > 0 && ((cp >= '0' && cp <= '9') || cp == '-')) ||
          (unit == 0 && cp == '-' && i + width < input.size());
      if (as_name) {
        out.append(input, i, width);
      } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "\\%x ", static_cast<unsigned>(cp));
        out += buf;
      }
      i += width;
      ++unit;
    }
    util::AddPerformanceCounter(util::PerfCounterId::CssEscapeCalls);
    return Value::String(std::move(out));
  });
  if (escape.IsObject()) {
    SetFunctionLength(escape, 1);
    css.object->Set("escape", escape);
  }

  if (js::Object* tag = interpreter_->SymbolToStringTag()) {
    js::Object::Property property;
    property.value = Value::String("CSS");
    property.writable = false;
    property.enumerable = false;
    property.configurable = true;
    css.object->Define(js::PropertyKey::Symbol(tag), std::move(property));
  }

  DefineNonEnumerable(interpreter_->Global(), "CSS", css);
  interpreter_->GlobalScope()->Declare("CSS", css, false);
}

}  // namespace microbrowser::bindings
