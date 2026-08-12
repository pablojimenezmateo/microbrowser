// `CSSStyleSheet`, `replaceSync`, and `adoptedStyleSheets`.
//
// ADR 0019 §4: one parsed sheet shared by every component instance rather than
// a `<style>` per instance. Lit on reddit and youtube depends on native
// constructable stylesheets when `attachShadow` exists and skips the polyfill.

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "css/StyleResolver.h"
#include "css/StyleSheet.h"
#include "dom/Node.h"
#include "util/PerformanceCounters.h"

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

}  // namespace

void DomBindings::InstallConstructableStylesheets(const js::Value& document_interface,
                                                  const js::Value& shadow_root_interface) {
  EnsureInterfaces();
  if (interpreter_ == nullptr) {
    return;
  }

  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  if (interfaces_.IsObject()) {
    interfaces_.object->Set("StyleSheet", prototype);
  }

  const Value replace_sync = interpreter_->NewNativeValue("replaceSync", [this](NativeCall& call) {
    SheetStorage* storage = SheetStoragePtr(call.self);
    if (storage == nullptr || *storage == nullptr) {
      return call.Throw("TypeError", "replaceSync called on an invalid CSSStyleSheet");
    }
    **storage = js::ToString(Argument(call.arguments, 0));
    if (document_ != nullptr) {
      document_->NoteTreeMutation();
    }
    return Value::Undefined();
  });
  if (replace_sync.IsObject()) {
    replace_sync.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("replaceSync", replace_sync);
  }

  const Value replace = interpreter_->NewNativeValue("replace", [](NativeCall& call) {
    SheetStorage* storage = SheetStoragePtr(call.self);
    if (storage == nullptr || *storage == nullptr) {
      return call.Throw("TypeError", "replace called on an invalid CSSStyleSheet");
    }
    // The async form can `@import`; absent rather than a sheet that silently
    // lacks its imports. ADR 0019 §4.
    const Value promise = call.interpreter.NewPromiseValue();
    if (!promise.IsObject()) {
      return Value::Undefined();
    }
    call.interpreter.SettleAsyncResult(
        promise.object,
        MakeDomException(call.interpreter, "NotSupportedError",
                         "CSSStyleSheet.replace is not supported"),
        true);
    return promise;
  });
  if (replace.IsObject()) {
    replace.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("replace", replace);
  }

  DomBindings* self = this;
  const Value constructor = interpreter_->NewNativeValue("CSSStyleSheet", [self](NativeCall& call) {
    const Value sheet = call.interpreter.NewObjectValue();
    if (!sheet.IsObject()) {
      return Value::Undefined();
    }
    if (const Value* style_sheet_proto = self->interfaces_.object->GetOwn("StyleSheet")) {
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
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    interpreter_->Global()->Set("CSSStyleSheet", constructor);
    interpreter_->GlobalScope()->Declare("CSSStyleSheet", constructor, false);
  }

  const auto install_adopted = [this](const js::Value& target) {
    if (!target.IsObject()) {
      return;
    }
    const Value getter = interpreter_->NewNativeValue("adoptedStyleSheets", [this](NativeCall& call) {
      dom::Node* root = AdoptedStyleRootOf(NodeOf(call.self));
      if (root == nullptr) {
        return call.interpreter.NewArrayValue({});
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
        if (const Value* style_sheet_proto = interfaces_.object->GetOwn("StyleSheet")) {
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
      target.object->DefineAccessor("adoptedStyleSheets", getter.object, setter.object);
    }
  };

  install_adopted(document_interface);
  install_adopted(shadow_root_interface);
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
    css.object->Set("escape", escape);
  }

  interpreter_->Global()->Set("CSS", css);
  interpreter_->GlobalScope()->Declare("CSS", css, false);
}

}  // namespace microbrowser::bindings
