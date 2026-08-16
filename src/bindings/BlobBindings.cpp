// `Blob` and `File`, plus `URL.createObjectURL(Blob)` for es-module-shims.
//
// Split out because DomBindings.cpp is at its cap. es-module-shims builds probe
// scripts with `new Blob([source], {type})` and loads them through blob URLs;
// without both halves the polyfill's `initPromise` never settles and concat
// bundles never run.
//
// `type` is the MIME Sniffing parse-then-serialize, not the string a page
// wrote: `new Blob([], {type: "TEXT/HTML;CHARSET=GBK"}).type` is
// `text/html;charset=GBK`, and a string that does not parse is the empty
// string. File is a Blob with a name, sharing the same slots so `blob()` on a
// Response and `new File` are the same shape.

#include <string>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "js/Interpreter.h"
#include "util/MimeType.h"

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
  return type == nullptr || !type->IsString() ? std::string() : type->AsString();
}

std::string TypeFromOptions(const Value& options) {
  if (!options.IsObject()) {
    return std::string();
  }
  const Value* named_type = options.object->Get("type");
  if (named_type == nullptr || named_type->IsUndefined()) {
    return std::string();
  }
  return util::BlobMimeType(js::ToString(*named_type));
}

std::string BodyFromParts(const Value& parts) {
  if (parts.IsUndefined() || parts.IsNull()) {
    return std::string();
  }
  if (!parts.IsObject()) {
    return js::ToString(parts);
  }
  std::string body;
  for (std::size_t i = 0; i < parts.object->ElementCount(); ++i) {
    body += js::ToString(parts.object->GetElement(i));
  }
  return body;
}

}  // namespace

js::Value MakeBlobValue(js::Interpreter& interpreter, std::string body, std::string type) {
  const Value object = interpreter.NewObjectValue();
  if (!object.IsObject()) {
    return object;
  }
  const Value* constructor = interpreter.Global()->Get("Blob");
  if (constructor != nullptr && constructor->IsObject()) {
    if (const Value* prototype = constructor->object->Get("prototype");
        prototype != nullptr && prototype->IsObject()) {
      object.object->SetPrototype(prototype->object);
    }
  }
  object.object->SetHidden(kBlobBodySlot, Value::String(std::move(body)));
  object.object->SetHidden(kBlobTypeSlot, Value::String(std::move(type)));
  object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
  return object;
}

void DomBindings::InstallBlob() {
  if (interpreter_ == nullptr) {
    return;
  }
  if (const Value* existing = interpreter_->Global()->Get("Blob"); existing != nullptr) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  const Value constructor = interpreter_->NewNativeValue("Blob", [prototype](NativeCall& call) {
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (prototype.IsObject()) {
      object.object->SetPrototype(prototype.object);
    }
    object.object->SetHidden(kBlobBodySlot, Value::String(BodyFromParts(Argument(call.arguments, 0))));
    object.object->SetHidden(kBlobTypeSlot, Value::String(TypeFromOptions(Argument(call.arguments, 1))));
    object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
    return object;
  });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, OwnerValue(this));
  if (prototype.IsObject()) {
    constructor.object->Set("prototype", prototype);
    const Value type_get = interpreter_->NewNativeValue("type", [](NativeCall& call) {
      return Value::String(BlobType(call.self));
    });
    if (type_get.IsObject()) {
      prototype.object->DefineAccessor("type", type_get.object, nullptr);
    }
  }
  interpreter_->Global()->Set("Blob", constructor);
  interpreter_->GlobalScope()->Declare("Blob", constructor, false);

  // File is a Blob with a name. Same type algorithm, same slots, so
  // `instanceof Blob` answers and `blob()` can return either.
  const Value file_prototype = interpreter_->NewObjectValue();
  if (file_prototype.IsObject() && prototype.IsObject()) {
    file_prototype.object->SetPrototype(prototype.object);
  }
  const Value file_constructor =
      interpreter_->NewNativeValue("File", [file_prototype](NativeCall& call) {
        const Value object = call.interpreter.NewObjectValue();
        if (!object.IsObject()) {
          return Value::Undefined();
        }
        if (file_prototype.IsObject()) {
          object.object->SetPrototype(file_prototype.object);
        }
        object.object->SetHidden(kBlobBodySlot,
                                 Value::String(BodyFromParts(Argument(call.arguments, 0))));
        object.object->SetHidden(kBlobTypeSlot,
                                 Value::String(TypeFromOptions(Argument(call.arguments, 2))));
        object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
        object.object->SetHidden(kBlobNameSlot, Value::String(js::ToString(Argument(call.arguments, 1))));
        return object;
      });
  if (file_constructor.IsObject()) {
    file_constructor.object->Set(kOwnerSlot, OwnerValue(this));
    if (file_prototype.IsObject()) {
      file_constructor.object->Set("prototype", file_prototype);
      const Value name_get = interpreter_->NewNativeValue("name", [](NativeCall& call) {
        const Value* name =
            call.self.IsObject() ? call.self.object->GetOwn(kBlobNameSlot) : nullptr;
        return name == nullptr || !name->IsString() ? Value::String(std::string()) : *name;
      });
      if (name_get.IsObject()) {
        file_prototype.object->DefineAccessor("name", name_get.object, nullptr);
      }
    }
    interpreter_->Global()->Set("File", file_constructor);
    interpreter_->GlobalScope()->Declare("File", file_constructor, false);
  }
}

bool DomBindings::IsBlobValue(const js::Value& value) const { return IsBlob(value); }

std::string DomBindings::BlobBodyOf(const js::Value& blob) const { return BlobBody(blob); }

std::string DomBindings::BlobTypeOf(const js::Value& blob) const { return BlobType(blob); }

}  // namespace microbrowser::bindings
