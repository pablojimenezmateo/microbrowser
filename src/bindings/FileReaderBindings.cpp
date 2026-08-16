// FileReader: an EventTarget that turns a Blob into text, bytes, or a data URL.
//
// Split out because BlobBindings.cpp is the constructor half and this is the
// async read half -- and because FileReader's events are macrotasks. loadstart
// and the rest must not share a turn: abort() after loadstart fires abort and
// loadend synchronously, which is only observable if progress/load have not
// already run. QueueTask for loadstart, QueueDelayedTask(0) for the rest, so
// the host-task drain cannot collapse the two. Workers have no TimerQueue and
// fall back to setTimeout(0), whose RunDue snapshots the same way.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
#include "html/Encoding.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "util/Base64.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

struct FileReaderAccess {
  static void FireProgress(DomBindings& bindings, const Value& reader, const char* type,
                           double size) {
    const Value event = bindings.MakeEvent(type, false, false, true);
    if (!event.IsObject()) {
      return;
    }
    if (const Value proto = bindings.EventPrototype("ProgressEvent", "Event"); proto.IsObject()) {
      event.object->SetPrototype(proto.object);
    }
    event.object->Set("target", reader);
    event.object->Set("loaded", Value::Number(size));
    event.object->Set("total", Value::Number(size));
    event.object->Set("lengthComputable", Value::Bool(true));
    const std::string handler_slot = std::string("#on") + type;
    if (const Value* handler = reader.object->GetOwn(handler_slot);
        handler != nullptr && handler->IsObject() && handler->object->IsCallable() &&
        bindings.interpreter_ != nullptr) {
      const js::Result outcome =
          bindings.interpreter_->CallFunction(*handler, reader, {event});
      if (outcome.completion == js::Completion::Throw) {
        bindings.interpreter_->ReportUncaught(outcome.value, "FileReader handler");
      }
    }
    bindings.RunListenersOn(reader, event, std::string("#on:") + type,
                            DomBindings::EventPhase::AtTarget);
  }
};

constexpr const char* kReadySlot = "#frReadyState";
constexpr const char* kResultSlot = "#frResult";
constexpr const char* kErrorSlot = "#frError";
constexpr const char* kGenSlot = "#frGeneration";
constexpr const char* kKindSlot = "#frKind";
constexpr const char* kBodySlot = "#frBody";
constexpr const char* kTypeSlot = "#frType";
constexpr const char* kEncSlot = "#frEncoding";

constexpr double kEmpty = 0;
constexpr double kLoading = 1;
constexpr double kDone = 2;

namespace {

double NumberSlot(const Value& object, const char* slot, double fallback) {
  if (!object.IsObject()) {
    return fallback;
  }
  const Value* found = object.object->GetOwn(slot);
  return found == nullptr || !found->IsNumber() ? fallback : found->number;
}

std::string StringSlot(const Value& object, const char* slot) {
  if (!object.IsObject()) {
    return {};
  }
  const Value* found = object.object->GetOwn(slot);
  return found == nullptr || !found->IsString() ? std::string() : found->AsString();
}

Value SlotOrNull(const Value& object, const char* slot) {
  if (!object.IsObject()) {
    return Value::Null();
  }
  const Value* found = object.object->GetOwn(slot);
  return found == nullptr ? Value::Null() : *found;
}

void SetSlot(const Value& object, const char* slot, const Value& value) {
  if (object.IsObject()) {
    object.object->SetHidden(slot, value);
  }
}

std::string Latin1String(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const char ch : bytes) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte < 0x80) {
      out.push_back(static_cast<char>(byte));
    } else {
      out.push_back(static_cast<char>(0xC0 | (byte >> 6)));
      out.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
    }
  }
  return out;
}

html::Encoding EncodingFor(std::string_view bytes, std::string_view type,
                           std::string_view label) {
  // BOM wins: it is in the bytes. A charset parameter is a claim about them.
  if (bytes.size() >= 3 && static_cast<std::uint8_t>(bytes[0]) == 0xEF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xBB && static_cast<std::uint8_t>(bytes[2]) == 0xBF) {
    return html::Encoding::Utf8;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFF &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFE) {
    return html::Encoding::Utf16Le;
  }
  if (bytes.size() >= 2 && static_cast<std::uint8_t>(bytes[0]) == 0xFE &&
      static_cast<std::uint8_t>(bytes[1]) == 0xFF) {
    return html::Encoding::Utf16Be;
  }
  if (!label.empty()) {
    if (const std::optional<html::Encoding> found = html::EncodingFromLabel(label)) {
      return *found;
    }
  }
  if (const std::optional<html::Encoding> found = html::EncodingFromMimeType(type)) {
    return *found;
  }
  return html::Encoding::Utf8;
}

Value ArrayBufferFromBytes(js::Interpreter& interpreter, std::string_view body) {
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

Value DecodeResult(js::Interpreter& interpreter, const Value& reader) {
  const std::string kind = StringSlot(reader, kKindSlot);
  const std::string body = StringSlot(reader, kBodySlot);
  const std::string type = StringSlot(reader, kTypeSlot);
  if (kind == "buffer") {
    return ArrayBufferFromBytes(interpreter, body);
  }
  if (kind == "binary") {
    return Value::String(Latin1String(body));
  }
  if (kind == "dataurl") {
    const std::string media = type.empty() ? std::string("application/octet-stream") : type;
    return Value::String("data:" + media + ";base64," + util::Base64Encode(body));
  }
  return Value::String(html::DecodeToUtf8(body, EncodingFor(body, type, StringSlot(reader, kEncSlot))));
}

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

bool QueueReaderTask(js::Interpreter& interpreter, const Value& callback, bool host) {
  if (host) {
    if (TimerQueue::QueueTask(interpreter, callback)) {
      return true;
    }
  } else if (TimerQueue::QueueDelayedTask(interpreter, callback, 0)) {
    return true;
  }
  const Value* set_timeout = interpreter.GlobalScope()->Lookup("setTimeout");
  if (set_timeout == nullptr || !set_timeout->IsObject()) {
    return false;
  }
  const js::Result outcome = interpreter.CallFunction(
      *set_timeout, Value::Obj(interpreter.Global()), {callback, Value::Number(0)});
  return !outcome.IsAbrupt();
}

void QueueCompletion(DomBindings* owner, js::Interpreter& interpreter, const Value& reader,
                     double generation, double size) {
  const Value complete = interpreter.NewNativeValue(
      "frComplete", [owner, reader, generation, size](NativeCall& call) -> Value {
        if (NumberSlot(reader, kGenSlot, 0) != generation ||
            NumberSlot(reader, kReadySlot, kEmpty) != kLoading) {
          return Value::Undefined();
        }
        if (size > 0) {
          FileReaderAccess::FireProgress(*owner, reader, "progress", size);
        }
        if (NumberSlot(reader, kGenSlot, 0) != generation ||
            NumberSlot(reader, kReadySlot, kEmpty) != kLoading) {
          return Value::Undefined();
        }
        SetSlot(reader, kResultSlot, DecodeResult(call.interpreter, reader));
        SetSlot(reader, kReadySlot, Value::Number(kDone));
        FileReaderAccess::FireProgress(*owner, reader, "load", size);
        FileReaderAccess::FireProgress(*owner, reader, "loadend", size);
        return Value::Undefined();
      });
  if (complete.IsObject()) {
    complete.object->Set("#reader", reader);
    QueueReaderTask(interpreter, complete, false);
  }
}

Value StartRead(DomBindings* owner, NativeCall& call, const char* kind) {
  if (!call.self.IsObject()) {
    return Value::Undefined();
  }
  if (NumberSlot(call.self, kReadySlot, kEmpty) == kLoading) {
    return ThrowDom(call, "InvalidStateError", "FileReader is already loading");
  }
  const Value blob = Argument(call.arguments, 0);
  if (owner == nullptr || !IsBlob(blob)) {
    return call.Throw("TypeError", "FileReader.read requires a Blob");
  }
  const std::string body = BlobBody(blob);
  const double size = static_cast<double>(body.size());
  const double generation = NumberSlot(call.self, kGenSlot, 0) + 1;
  SetSlot(call.self, kGenSlot, Value::Number(generation));
  SetSlot(call.self, kReadySlot, Value::Number(kLoading));
  SetSlot(call.self, kResultSlot, Value::Null());
  SetSlot(call.self, kErrorSlot, Value::Null());
  SetSlot(call.self, kKindSlot, Value::String(kind));
  SetSlot(call.self, kBodySlot, Value::String(body));
  SetSlot(call.self, kTypeSlot, Value::String(BlobType(blob)));
  if (std::string_view(kind) == "text" && call.arguments.size() >= 2 &&
      !call.arguments[1].IsUndefined()) {
    SetSlot(call.self, kEncSlot, Value::String(js::ToString(call.arguments[1])));
  } else {
    SetSlot(call.self, kEncSlot, Value::String(""));
  }

  const Value reader = call.self;
  const Value loadstart = call.interpreter.NewNativeValue(
      "frLoadStart", [owner, reader, generation, size](NativeCall& inner) -> Value {
        if (NumberSlot(reader, kGenSlot, 0) != generation ||
            NumberSlot(reader, kReadySlot, kEmpty) != kLoading) {
          return Value::Undefined();
        }
        FileReaderAccess::FireProgress(*owner, reader, "loadstart", size);
        if (NumberSlot(reader, kGenSlot, 0) != generation ||
            NumberSlot(reader, kReadySlot, kEmpty) != kLoading) {
          return Value::Undefined();
        }
        QueueCompletion(owner, inner.interpreter, reader, generation, size);
        return Value::Undefined();
      });
  if (loadstart.IsObject()) {
    loadstart.object->Set("#reader", reader);
    QueueReaderTask(call.interpreter, loadstart, true);
  }
  return Value::Undefined();
}

}  // namespace

void DomBindings::InstallFileReader() {
  if (interpreter_ == nullptr) {
    return;
  }
  EnsureInterfaces();
  const Value prototype = MakeInterface("FileReader", InterfaceNamed("EventTarget"));
  if (!prototype.IsObject()) {
    return;
  }

  DomBindings* self = this;
  js::Interpreter& interpreter = *interpreter_;
  const auto add = [self, &interpreter, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter.NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(self));
      prototype.object->Set(name, native);
    }
  };

  const Value ready_get = interpreter.NewNativeValue("readyState", [](NativeCall& call) {
    return Value::Number(NumberSlot(call.self, kReadySlot, kEmpty));
  });
  const Value result_get = interpreter.NewNativeValue("result", [](NativeCall& call) {
    return SlotOrNull(call.self, kResultSlot);
  });
  const Value error_get = interpreter.NewNativeValue("error", [](NativeCall& call) {
    return SlotOrNull(call.self, kErrorSlot);
  });
  if (ready_get.IsObject()) {
    prototype.object->DefineAccessor("readyState", ready_get.object, nullptr);
  }
  if (result_get.IsObject()) {
    prototype.object->DefineAccessor("result", result_get.object, nullptr);
  }
  if (error_get.IsObject()) {
    prototype.object->DefineAccessor("error", error_get.object, nullptr);
  }

  add("readAsText", [](NativeCall& call) -> Value { return StartRead(OwnerOf(call), call, "text"); });
  add("readAsArrayBuffer",
      [](NativeCall& call) -> Value { return StartRead(OwnerOf(call), call, "buffer"); });
  add("readAsDataURL",
      [](NativeCall& call) -> Value { return StartRead(OwnerOf(call), call, "dataurl"); });
  add("readAsBinaryString",
      [](NativeCall& call) -> Value { return StartRead(OwnerOf(call), call, "binary"); });
  add("abort", [](NativeCall& call) -> Value {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    if (NumberSlot(call.self, kReadySlot, kEmpty) != kLoading) {
      return Value::Undefined();
    }
    DomBindings* owner = OwnerOf(call);
    const double size = static_cast<double>(StringSlot(call.self, kBodySlot).size());
    SetSlot(call.self, kGenSlot, Value::Number(NumberSlot(call.self, kGenSlot, 0) + 1));
    SetSlot(call.self, kReadySlot, Value::Number(kDone));
    SetSlot(call.self, kResultSlot, Value::Null());
    if (owner != nullptr) {
      SetSlot(call.self, kErrorSlot,
              MakeDomException(call.interpreter, "AbortError", "The operation was aborted."));
      FileReaderAccess::FireProgress(*owner, call.self, "abort", size);
      FileReaderAccess::FireProgress(*owner, call.self, "loadend", size);
    }
    return Value::Undefined();
  });

  for (const char* name :
       {"onloadstart", "onprogress", "onload", "onabort", "onerror", "onloadend"}) {
    InstallOnEventAccessor(prototype, name);
  }

  const auto add_constants = [](const Value& target) {
    if (!target.IsObject()) {
      return;
    }
    target.object->Set("EMPTY", Value::Number(kEmpty));
    target.object->Set("LOADING", Value::Number(kLoading));
    target.object->Set("DONE", Value::Number(kDone));
  };
  add_constants(prototype);

  const Value constructor = interpreter.NewNativeValue(
      "FileReader", [self, prototype](NativeCall& call) -> Value {
        if (!call.interpreter.IsConstructCall(call.self)) {
          return call.Throw("TypeError", "FileReader constructor requires 'new'");
        }
        const Value reader = call.interpreter.NewObjectValue();
        if (!reader.IsObject()) {
          return Value::Undefined();
        }
        if (prototype.IsObject()) {
          reader.object->SetPrototype(prototype.object);
        }
        SetSlot(reader, kReadySlot, Value::Number(kEmpty));
        SetSlot(reader, kResultSlot, Value::Null());
        SetSlot(reader, kErrorSlot, Value::Null());
        SetSlot(reader, kGenSlot, Value::Number(0));
        self->InstallEventMethods(reader);
        return reader;
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, OwnerValue(self));
  constructor.object->Set("length", Value::Number(0));
  constructor.object->Set("prototype", prototype);
  prototype.object->Set("constructor", constructor);
  add_constants(constructor);
  interpreter.Global()->Set("FileReader", constructor);
  interpreter.GlobalScope()->Declare("FileReader", constructor, false);
}

}  // namespace microbrowser::bindings
