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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "util/MimeType.h"
#include "util/StringUtil.h"

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

void AppendBufferBytes(std::string& body, const Value& part) {
  if (!part.IsObject()) {
    return;
  }
  const js::BufferView* view = part.object->View();
  if (view == nullptr || view->bytes == nullptr) {
    return;  // detached: contribute nothing
  }
  const std::size_t element_size = js::ElementSize(view->kind);
  const std::size_t byte_length = view->length * element_size;
  if (view->offset > view->bytes->size() || byte_length > view->bytes->size() - view->offset) {
    return;
  }
  body.append(reinterpret_cast<const char*>(view->bytes->data() + view->offset), byte_length);
}

void AppendPart(std::string& body, const Value& part) {
  if (IsBlob(part)) {
    body += BlobBody(part);
    return;
  }
  if (part.IsObject() && part.object->View() != nullptr) {
    AppendBufferBytes(body, part);
    return;
  }
  body += js::ToString(part);
}

bool BodyFromBlobParts(NativeCall& call, const Value& parts, std::string& body) {
  if (parts.IsUndefined()) {
    return true;
  }
  if (parts.IsNull() || !parts.IsObject()) {
    (void)call.Throw("TypeError", "Blob parts must be a sequence");
    return false;
  }
  std::vector<Value> items;
  if (!IterateValue(call, parts, items)) {
    return false;
  }
  for (const Value& item : items) {
    AppendPart(body, item);
  }
  return true;
}

bool OptionsType(NativeCall& call, const Value& options, std::string& type) {
  if (options.IsUndefined() || options.IsNull()) {
    type.clear();
    return true;
  }
  if (!options.IsObject()) {
    (void)call.Throw("TypeError", "Blob options must be an object");
    return false;
  }
  type = TypeFromOptions(options);
  return true;
}

std::int64_t ToSliceOffset(const Value& value) {
  const double number = js::ToNumber(value);
  if (!std::isfinite(number)) {
    return 0;
  }
  if (number >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (number <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  // WebIDL long long: round to nearest, ties to even. Truncation would make
  // `blob.slice(1.5)` of "abcd" start at 1 ("bcd") instead of 2 ("cd").
  return static_cast<std::int64_t>(std::nearbyint(number));
}

std::size_t NormalizeSlice(std::int64_t relative, std::size_t size) {
  if (relative < 0) {
    const std::int64_t shifted = static_cast<std::int64_t>(size) + relative;
    return shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
  }
  return static_cast<std::uint64_t>(relative) > size ? size : static_cast<std::size_t>(relative);
}

// File API slice type: ASCII 0x20-0x7E, then lowercased. Not the MIME parser
// the constructor uses -- `slice(0,0,null).type` is `"null"`.
std::string SliceContentType(const Value& value) {
  if (value.IsUndefined()) {
    return std::string();
  }
  const std::string type = js::ToString(value);
  for (const char ch : type) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c > 0x7E) {
      return std::string();
    }
  }
  return util::AsciiLowerCase(type);
}

js::Value Settled(js::Interpreter& interpreter, const Value& value) {
  const Value promise = interpreter.NewPromiseValue();
  if (promise.IsObject()) {
    interpreter.SettleAsyncResult(promise.object, value, false);
  }
  return promise;
}

js::Value ArrayBufferFromBytes(js::Interpreter& interpreter, std::string_view body) {
  const Value* buffer_ctor = interpreter.GlobalScope()->Lookup("ArrayBuffer");
  if (buffer_ctor == nullptr) {
    return interpreter.MakeError("TypeError", "ArrayBuffer is unavailable");
  }
  const js::Result buffer = interpreter.ConstructValue(
      *buffer_ctor, {Value::Number(static_cast<double>(body.size()))});
  if (buffer.IsAbrupt() || !buffer.value.IsObject()) {
    return buffer.IsAbrupt() ? buffer.value
                             : interpreter.MakeError("TypeError", "failed to allocate ArrayBuffer");
  }
  const js::BufferView* view = buffer.value.object->View();
  if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= body.size()) {
    std::memcpy(view->bytes->data(), body.data(), body.size());
  }
  return buffer.value;
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
    std::string body;
    std::string type;
    if (!BodyFromBlobParts(call, Argument(call.arguments, 0), body) ||
        !OptionsType(call, Argument(call.arguments, 1), type)) {
      return call.ThrownValue();
    }
    object.object->SetHidden(kBlobBodySlot, Value::String(std::move(body)));
    object.object->SetHidden(kBlobTypeSlot, Value::String(std::move(type)));
    object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
    return object;
  });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, OwnerValue(this));
  constructor.object->Set("length", Value::Number(0));
  if (prototype.IsObject()) {
    constructor.object->Set("prototype", prototype);
    const Value type_get = interpreter_->NewNativeValue("type", [](NativeCall& call) {
      return Value::String(BlobType(call.self));
    });
    if (type_get.IsObject()) {
      prototype.object->DefineAccessor("type", type_get.object, nullptr);
    }
    const Value size_get = interpreter_->NewNativeValue("size", [](NativeCall& call) {
      return Value::Number(static_cast<double>(BlobBody(call.self).size()));
    });
    if (size_get.IsObject()) {
      prototype.object->DefineAccessor("size", size_get.object, nullptr);
    }
    const Value slice = interpreter_->NewNativeValue("slice", [](NativeCall& call) -> Value {
      const std::string body = BlobBody(call.self);
      const std::size_t size = body.size();
      std::int64_t start_rel = 0;
      std::int64_t end_rel = static_cast<std::int64_t>(size);
      if (call.arguments.size() >= 1 && !call.arguments[0].IsUndefined()) {
        start_rel = ToSliceOffset(call.arguments[0]);
      }
      if (call.arguments.size() >= 2 && !call.arguments[1].IsUndefined()) {
        end_rel = ToSliceOffset(call.arguments[1]);
      }
      const std::size_t from = NormalizeSlice(start_rel, size);
      const std::size_t to = NormalizeSlice(end_rel, size);
      const std::size_t span = from < to ? to - from : 0;
      std::string type;
      if (call.arguments.size() >= 3) {
        type = SliceContentType(call.arguments[2]);
      }
      return MakeBlobValue(call.interpreter, body.substr(from, span), std::move(type));
    });
    if (slice.IsObject()) {
      prototype.object->Set("slice", slice);
    }
    const Value text = interpreter_->NewNativeValue("text", [](NativeCall& call) {
      return Settled(call.interpreter, Value::String(util::Utf8DecodeLossy(BlobBody(call.self))));
    });
    if (text.IsObject()) {
      prototype.object->Set("text", text);
    }
    const Value array_buffer = interpreter_->NewNativeValue("arrayBuffer", [](NativeCall& call) {
      const Value buffer = ArrayBufferFromBytes(call.interpreter, BlobBody(call.self));
      if (!buffer.IsObject()) {
        const Value promise = call.interpreter.NewPromiseValue();
        if (promise.IsObject()) {
          call.interpreter.SettleAsyncResult(promise.object, buffer, true);
        }
        return promise;
      }
      return Settled(call.interpreter, buffer);
    });
    if (array_buffer.IsObject()) {
      prototype.object->Set("arrayBuffer", array_buffer);
    }
    if (js::Object* tag = interpreter_->SymbolToStringTag()) {
      prototype.object->SetHidden(js::PropertyKey::Symbol(tag), Value::String("Blob"));
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
        std::string body;
        std::string type;
        if (!BodyFromBlobParts(call, Argument(call.arguments, 0), body) ||
            !OptionsType(call, Argument(call.arguments, 2), type)) {
          return call.ThrownValue();
        }
        object.object->SetHidden(kBlobBodySlot, Value::String(std::move(body)));
        object.object->SetHidden(kBlobTypeSlot, Value::String(std::move(type)));
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
      if (js::Object* tag = interpreter_->SymbolToStringTag()) {
        file_prototype.object->SetHidden(js::PropertyKey::Symbol(tag), Value::String("File"));
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
