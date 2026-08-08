// `CSSStyleSheet`, `replaceSync`, and `adoptedStyleSheets`.
//
// ADR 0019 §4: one parsed sheet shared by every component instance rather than
// a `<style>` per instance. Lit on reddit and youtube depends on native
// constructable stylesheets when `attachShadow` exists and skips the polyfill.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
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
    replace_sync.object->Set(kOwnerSlot, PointerValue(this));
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
        call.interpreter.MakeError("NotSupportedError", "CSSStyleSheet.replace is not supported"),
        true);
    return promise;
  });
  if (replace.IsObject()) {
    replace.object->Set(kOwnerSlot, PointerValue(this));
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
    auto storage = std::make_unique<SheetStorage>(std::make_shared<std::string>());
    const auto* heap = storage.release();
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
    constructor.object->Set(kOwnerSlot, PointerValue(this));
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
        auto storage = std::make_unique<SheetStorage>(text);
        sheet.object->SetHidden(kCSSStyleSheetMarkerSlot, Value::Bool(true));
        sheet.object->SetHidden(kCSSSheetStorageSlot,
                                PointerValue(storage.release()));
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
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor("adoptedStyleSheets", getter.object, setter.object);
    }
  };

  install_adopted(document_interface);
  install_adopted(shadow_root_interface);
}

}  // namespace microbrowser::bindings
