// `Blob` and `File`, plus `URL.createObjectURL(Blob)` for es-module-shims.
//
// Split out because DomBindings.cpp is at its cap. es-module-shims builds probe
// scripts with `new Blob([source], {type})` and loads them through blob URLs;
// without both halves the polyfill's `initPromise` never settles and concat
// bundles never run.
//
// File API `type` is printable ASCII 0x20-0x7E, then lowercased -- the same
// algorithm as `slice`, and not the MIME parser. `new Blob([], {type: "A"}).type`
// is `"a"`, and `" image/gif "` keeps its spaces. File is a Blob with a name,
// sharing the same slots so `blob()` on a Response and `new File` are the same
// shape.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
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

// File API type: ASCII 0x20-0x7E, then lowercased. A tab, a DEL, or a
// non-ASCII code point empties the type rather than being stripped.
std::string CanonicalContentType(std::string_view type) {
  for (const char ch : type) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c > 0x7E) {
      return std::string();
    }
  }
  return util::AsciiLowerCase(std::string(type));
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

bool AppendPart(NativeCall& call, std::vector<std::pair<std::string, bool>>& parts,
                const Value& part) {
  if (IsBlob(part)) {
    parts.emplace_back(BlobBody(part), false);
    return true;
  }
  if (part.IsObject() && part.object->View() != nullptr) {
    std::string bytes;
    AppendBufferBytes(bytes, part);
    parts.emplace_back(std::move(bytes), false);
    return true;
  }
  // The interpreter's ToString: a part's own `toString` has to run, and a throw
  // from it has to stop the constructor before `options.type` is read.
  std::string text;
  const js::Result converted = call.interpreter.ToStringOf(part, text);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  parts.emplace_back(std::move(text), true);
  return true;
}

bool BodyFromBlobParts(NativeCall& call, const Value& parts_value,
                       std::vector<std::pair<std::string, bool>>& parts) {
  if (parts_value.IsUndefined()) {
    return true;
  }
  if (parts_value.IsNull() || !parts_value.IsObject()) {
    (void)call.Throw("TypeError", "Blob parts must be a sequence");
    return false;
  }
  // Convert each yielded value before asking for the next. Prefetching the
  // whole sequence and then ToString-ing it would miss a `pop()` inside the
  // first element's `toString`, which the iterator is specified to observe.
  //
  // `@@iterator` is a [[Get]]: a getter has to run, and `Object::Get` skips
  // accessors, which made the constructor throw TypeError instead of the
  // exception the getter's length-or-index neighbour threw.
  js::Result abrupt = js::Result::Normal();
  const Value method = call.interpreter.GetPropertyOrThrow(
      parts_value, js::PropertyKey::Symbol(call.interpreter.SymbolIterator()), abrupt);
  if (abrupt.IsAbrupt()) {
    (void)call.ThrowValue(abrupt.value);
    return false;
  }
  if (!method.IsObject() || !method.object->IsCallable()) {
    (void)call.Throw("TypeError", "value is not iterable");
    return false;
  }
  const js::Result iterator = call.interpreter.CallFunction(method, parts_value, {});
  if (iterator.IsAbrupt()) {
    (void)call.ThrowValue(iterator.value);
    return false;
  }
  if (!iterator.value.IsObject()) {
    (void)call.Throw("TypeError", "iterator is not an object");
    return false;
  }
  const Value* next = iterator.value.object->Get("next");
  if (next == nullptr) {
    (void)call.Throw("TypeError", "iterator has no next()");
    return false;
  }
  constexpr std::size_t kMaxSteps = 1u << 22;
  for (std::size_t step = 0; step < kMaxSteps; ++step) {
    const js::Result stepped = call.interpreter.CallFunction(*next, iterator.value, {});
    if (stepped.IsAbrupt()) {
      (void)call.ThrowValue(stepped.value);
      return false;
    }
    if (!stepped.value.IsObject()) {
      (void)call.Throw("TypeError", "iterator result is not an object");
      return false;
    }
    const Value* done = stepped.value.object->Get("done");
    if (done != nullptr && js::ToBoolean(*done)) {
      return true;
    }
    const Value* item = stepped.value.object->Get("value");
    if (!AppendPart(call, parts, item == nullptr ? Value::Undefined() : *item)) {
      return false;
    }
  }
  (void)call.Throw("TypeError", "iterator did not finish");
  return false;
}

bool ReadDictionaryMember(NativeCall& call, const Value& options, const char* name,
                          std::string& text, bool& present) {
  js::Result abrupt = js::Result::Normal();
  const Value value = call.interpreter.GetPropertyOrThrow(options, name, abrupt);
  if (abrupt.IsAbrupt()) {
    (void)call.ThrowValue(abrupt.value);
    return false;
  }
  present = !value.IsUndefined();
  if (!present) {
    text.clear();
    return true;
  }
  const js::Result converted = call.interpreter.ToStringOf(value, text);
  if (converted.IsAbrupt()) {
    (void)call.ThrowValue(converted.value);
    return false;
  }
  return true;
}

bool OptionsType(NativeCall& call, const Value& options, std::string& type,
                 std::int64_t* last_modified, bool* native_endings) {
  if (options.IsUndefined() || options.IsNull()) {
    type.clear();
    return true;
  }
  if (!options.IsObject()) {
    (void)call.Throw("TypeError", "Blob options must be an object");
    return false;
  }
  // WebIDL dictionary order is lexicographic on the flattened bag:
  // `endings`, then `lastModified` (File), then `type`.
  std::string endings;
  bool has_endings = false;
  if (!ReadDictionaryMember(call, options, "endings", endings, has_endings)) {
    return false;
  }
  if (has_endings && endings != "native" && endings != "transparent") {
    (void)call.Throw("TypeError", "endings must be \"native\" or \"transparent\"");
    return false;
  }
  if (native_endings != nullptr) {
    *native_endings = has_endings && endings == "native";
  }
  if (last_modified != nullptr) {
    js::Result abrupt = js::Result::Normal();
    const Value value =
        call.interpreter.GetPropertyOrThrow(options, "lastModified", abrupt);
    if (abrupt.IsAbrupt()) {
      (void)call.ThrowValue(abrupt.value);
      return false;
    }
    if (!value.IsUndefined()) {
      double number = 0;
      const js::Result converted = call.interpreter.ToNumberOf(value, number);
      if (converted.IsAbrupt()) {
        (void)call.ThrowValue(converted.value);
        return false;
      }
      if (!std::isfinite(number)) {
        *last_modified = 0;
      } else if (number >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        *last_modified = std::numeric_limits<std::int64_t>::max();
      } else if (number <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        *last_modified = std::numeric_limits<std::int64_t>::min();
      } else {
        *last_modified = static_cast<std::int64_t>(std::nearbyint(number));
      }
    }
  }
  std::string raw;
  bool has_type = false;
  if (!ReadDictionaryMember(call, options, "type", raw, has_type)) {
    return false;
  }
  type = has_type ? CanonicalContentType(raw) : std::string();
  return true;
}

// File API "convert line endings to native". Each \r\n, \r, or \n becomes one
// native newline. Applied per string part, so ['\r','\n'] is two newlines.
std::string ConvertLineEndingsNative(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      out.push_back('\n');
      i += 2;
    } else if (text[i] == '\r' || text[i] == '\n') {
      out.push_back('\n');
      ++i;
    } else {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

std::string JoinParts(const std::vector<std::pair<std::string, bool>>& parts, bool native) {
  std::string body;
  for (const auto& [bytes, convert] : parts) {
    if (native && convert) {
      body += ConvertLineEndingsNative(bytes);
    } else {
      body += bytes;
    }
  }
  return body;
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

// `slice`'s contentType argument: same printable-ASCII rule as the constructor,
// including `slice(0,0,null).type === "null"`.
std::string SliceContentType(const Value& value) {
  if (value.IsUndefined()) {
    return std::string();
  }
  return CanonicalContentType(js::ToString(value));
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
    if (!call.interpreter.IsConstructCall(call.self)) {
      return call.Throw("TypeError", "Blob constructor requires 'new'");
    }
    const Value object = call.interpreter.NewObjectValue();
    if (!object.IsObject()) {
      return Value::Undefined();
    }
    if (prototype.IsObject()) {
      object.object->SetPrototype(prototype.object);
    }
    std::vector<std::pair<std::string, bool>> parts;
    std::string type;
    bool native = false;
    if (!BodyFromBlobParts(call, Argument(call.arguments, 0), parts) ||
        !OptionsType(call, Argument(call.arguments, 1), type, nullptr, &native)) {
      return call.ThrownValue();
    }
    std::string body = JoinParts(parts, native);
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
        if (!call.interpreter.IsConstructCall(call.self)) {
          return call.Throw("TypeError", "File constructor requires 'new'");
        }
        if (call.arguments.size() < 2) {
          return call.Throw("TypeError", "File requires bits and name");
        }
        const Value object = call.interpreter.NewObjectValue();
        if (!object.IsObject()) {
          return Value::Undefined();
        }
        if (file_prototype.IsObject()) {
          object.object->SetPrototype(file_prototype.object);
        }
        std::vector<std::pair<std::string, bool>> parts;
        std::string type;
        std::int64_t last_modified =
            static_cast<std::int64_t>(call.interpreter.NowMilliseconds());
        bool native = false;
        if (!BodyFromBlobParts(call, Argument(call.arguments, 0), parts)) {
          return call.ThrownValue();
        }
        std::string name;
        const js::Result named = call.interpreter.ToStringOf(call.arguments[1], name);
        if (named.IsAbrupt()) {
          return call.ThrowValue(named.value);
        }
        if (!OptionsType(call, Argument(call.arguments, 2), type, &last_modified, &native)) {
          return call.ThrownValue();
        }
        std::string body = JoinParts(parts, native);
        object.object->SetHidden(kBlobBodySlot, Value::String(std::move(body)));
        object.object->SetHidden(kBlobTypeSlot, Value::String(std::move(type)));
        object.object->SetHidden(kBlobMarkerSlot, Value::Bool(true));
        object.object->SetHidden(kBlobNameSlot, Value::String(std::move(name)));
        object.object->SetHidden(kBlobLastModifiedSlot,
                                 Value::Number(static_cast<double>(last_modified)));
        return object;
      });
  if (file_constructor.IsObject()) {
    file_constructor.object->Set(kOwnerSlot, OwnerValue(this));
    file_constructor.object->Set("length", Value::Number(2));
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
      const Value modified_get = interpreter_->NewNativeValue("lastModified", [](NativeCall& call) {
        const Value* at =
            call.self.IsObject() ? call.self.object->GetOwn(kBlobLastModifiedSlot) : nullptr;
        return at == nullptr || !at->IsNumber() ? Value::Number(0) : *at;
      });
      if (modified_get.IsObject()) {
        file_prototype.object->DefineAccessor("lastModified", modified_get.object, nullptr);
      }
      if (js::Object* tag = interpreter_->SymbolToStringTag()) {
        file_prototype.object->SetHidden(js::PropertyKey::Symbol(tag), Value::String("File"));
      }
    }
    interpreter_->Global()->Set("File", file_constructor);
    interpreter_->GlobalScope()->Declare("File", file_constructor, false);
  }
  InstallFileReader();
}

bool DomBindings::IsBlobValue(const js::Value& value) const { return IsBlob(value); }

std::string DomBindings::BlobBodyOf(const js::Value& blob) const { return BlobBody(blob); }

std::string DomBindings::BlobTypeOf(const js::Value& blob) const { return BlobType(blob); }

}  // namespace microbrowser::bindings
