// `Blob` and `URL.createObjectURL(Blob)` for es-module-shims feature detection.
//
// Split out because DomBindings.cpp is at its cap. es-module-shims builds probe
// scripts with `new Blob([source], {type})` and loads them through blob URLs;
// without both halves the polyfill's `initPromise` never settles and concat
// bundles never run.

#include <string>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "js/Interpreter.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

bool IsBlob(const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* marker = value.object->GetOwn(kBlobMarkerSlot);
  return marker != nullptr && marker->type == js::ValueType::Boolean && marker->boolean;
}

std::string BlobBody(const Value& blob) {
  const Value* body = blob.object->GetOwn(kBlobBodySlot);
  return body == nullptr || !body->IsString() ? std::string() : body->AsString();
}

std::string BlobType(const Value& blob) {
  const Value* type = blob.object->GetOwn(kBlobTypeSlot);
  return type == nullptr || !type->IsString() ? std::string("application/octet-stream")
                                                : type->AsString();
}

}  // namespace

void DomBindings::InstallBlob() {
  if (interpreter_ == nullptr) {
    return;
  }
  if (const Value* existing = interpreter_->Global()->Get("Blob"); existing != nullptr) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  const Value constructor = interpreter_->NewNativeValue("Blob", [prototype](NativeCall& call) {
    std::string body;
    std::string type = "application/octet-stream";
    if (!call.arguments.empty() && !Argument(call.arguments, 0).IsUndefined()) {
      const Value parts = Argument(call.arguments, 0);
      if (parts.IsObject()) {
        for (std::size_t i = 0; i < parts.object->ElementCount(); ++i) {
          body += js::ToString(parts.object->GetElement(i));
        }
      } else {
        body = js::ToString(parts);
      }
    }
    if (call.arguments.size() > 1 && Argument(call.arguments, 1).IsObject()) {
      const Value* named_type = Argument(call.arguments, 1).object->Get("type");
      if (named_type != nullptr && named_type->IsString() && !named_type->AsString().empty()) {
        type = named_type->AsString();
      }
    }
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (prototype.IsObject()) {
      object.object->SetPrototype(prototype.object);
    }
    object.object->SetHidden(kBlobBodySlot, Value::String(std::move(body)));
    object.object->SetHidden(kBlobTypeSlot, Value::String(std::move(type)));
    object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
    return object;
  });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, PointerValue(this));
  if (prototype.IsObject()) {
    constructor.object->Set("prototype", prototype);
  }
  interpreter_->Global()->Set("Blob", constructor);
  interpreter_->GlobalScope()->Declare("Blob", constructor, false);
}

bool DomBindings::IsBlobValue(const js::Value& value) const { return IsBlob(value); }

std::string DomBindings::BlobBodyOf(const js::Value& blob) const { return BlobBody(blob); }

std::string DomBindings::BlobTypeOf(const js::Value& blob) const { return BlobType(blob); }

}  // namespace microbrowser::bindings
