// `ReadableStream` / `ReadableStreamDefaultReader` for `Response.body`.
//
// Split from FetchTypes.cpp because that file was at the TU cap, and because
// the stream's *identity* is a different question from Headers/Response: SABR
// and every `body instanceof ReadableStream` check need a real prototype
// chain, not a plain object with an own `getReader`. Construction stays
// illegal (`new ReadableStream({start})`) until the controller model exists
// (ADR 0020) -- feature detection sees the global; construction does not
// invent a stub.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/FetchSupport.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

using ::microbrowser::bindings::SettledPromise;

Value BytesToUint8Array(js::Interpreter& interpreter, const std::string& body) {
  const Value* buffer_ctor = interpreter.GlobalScope()->Lookup("ArrayBuffer");
  const Value* view_ctor = interpreter.GlobalScope()->Lookup("Uint8Array");
  if (buffer_ctor == nullptr || view_ctor == nullptr) {
    return interpreter.MakeError("TypeError", "Uint8Array is unavailable");
  }
  const js::Result buffer = interpreter.ConstructValue(
      *buffer_ctor, {Value::Number(static_cast<double>(body.size()))});
  if (buffer.IsAbrupt() || !buffer.value.IsObject()) {
    return buffer.value;
  }
  const js::BufferView* view = buffer.value.object->View();
  if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= body.size()) {
    std::copy(body.begin(), body.end(), view->bytes->begin());
  }
  const js::Result array = interpreter.ConstructValue(*view_ctor, {buffer.value});
  if (array.IsAbrupt() || !array.value.IsObject()) {
    return array.value;
  }
  return array.value;
}

js::Object* StreamPrototype(js::Interpreter& interpreter) {
  const Value* ctor = interpreter.GlobalScope()->Lookup("ReadableStream");
  if (ctor == nullptr || !ctor->IsObject()) {
    return nullptr;
  }
  const Value* proto = ctor->object->Get("prototype");
  return proto != nullptr && proto->IsObject() ? proto->object : nullptr;
}

js::Object* ReaderPrototype(js::Interpreter& interpreter) {
  const Value* ctor = interpreter.GlobalScope()->Lookup("ReadableStreamDefaultReader");
  if (ctor == nullptr || !ctor->IsObject()) {
    return nullptr;
  }
  const Value* proto = ctor->object->Get("prototype");
  return proto != nullptr && proto->IsObject() ? proto->object : nullptr;
}

}  // namespace

js::Value MakeBodyStream(js::Interpreter& interpreter, const js::Value& response) {
  const Value stream = interpreter.NewObjectValue();
  if (!stream.IsObject() || !response.IsObject()) {
    return Value::Undefined();
  }
  if (js::Object* proto = StreamPrototype(interpreter)) {
    stream.object->SetPrototype(proto);
  }
  stream.object->SetHidden(kStreamResponseSlot, response);
  // Brand: `getReader` refuses a plain object that happens to sit on the
  // prototype chain via a page's own `Object.setPrototypeOf`.
  stream.object->SetHidden(kStreamBrandSlot, Value::Bool(true));
  return stream;
}

void DomBindings::InstallReadableStream() {
  EnsureInterfaces();
  if (interpreter_->GlobalScope()->Lookup("ReadableStream") != nullptr) {
    return;
  }

  const Value stream_proto = interpreter_->NewObjectValue();
  const Value reader_proto = interpreter_->NewObjectValue();
  if (!stream_proto.IsObject() || !reader_proto.IsObject()) {
    return;
  }

  const Value get_reader = interpreter_->NewNativeValue(
      "getReader", [](NativeCall& stream_call) -> Value {
        if (!stream_call.self.IsObject() ||
            stream_call.self.object->GetOwn(kStreamBrandSlot) == nullptr) {
          return stream_call.Throw("TypeError", "not a ReadableStream");
        }
        const Value* response_value = stream_call.self.object->GetOwn(kStreamResponseSlot);
        if (response_value == nullptr || !response_value->IsObject()) {
          return stream_call.Throw("TypeError", "not a ReadableStream");
        }
        const Value* locked = response_value->object->GetOwn(kBodyLockedSlot);
        if (locked != nullptr && js::ToBoolean(*locked)) {
          return stream_call.Throw("TypeError", "ReadableStream is locked");
        }
        const Value* used = response_value->object->GetOwn(kBodyUsedSlot);
        if (used != nullptr && js::ToBoolean(*used)) {
          return stream_call.Throw("TypeError", "body already read");
        }
        response_value->object->SetHidden(kBodyLockedSlot, Value::Bool(true));

        const Value reader = stream_call.interpreter.NewObjectValue();
        if (!reader.IsObject()) {
          return Value::Undefined();
        }
        if (js::Object* proto = ReaderPrototype(stream_call.interpreter)) {
          reader.object->SetPrototype(proto);
        }
        reader.object->SetHidden(kStreamResponseSlot, *response_value);
        reader.object->SetHidden(kReaderDoneSlot, Value::Bool(false));
        reader.object->SetHidden(kReaderBrandSlot, Value::Bool(true));
        reader.object->SetHidden(kReaderStreamSlot, stream_call.self);
        return reader;
      });
  if (get_reader.IsObject()) {
    stream_proto.object->Set("getReader", get_reader);
  }

  const Value stream_cancel = interpreter_->NewNativeValue(
      "cancel", [](NativeCall& call) -> Value {
        if (!call.self.IsObject() || call.self.object->GetOwn(kStreamBrandSlot) == nullptr) {
          return SettledPromise(
              call.interpreter, call.interpreter.MakeError("TypeError", "not a ReadableStream"),
              true);
        }
        if (const Value* response_value = call.self.object->GetOwn(kStreamResponseSlot)) {
          if (response_value->IsObject()) {
            response_value->object->SetHidden(kBodyUsedSlot, Value::Bool(true));
            response_value->object->SetHidden(kBodyLockedSlot, Value::Bool(false));
          }
        }
        return SettledPromise(call.interpreter, Value::Undefined(), false);
      });
  if (stream_cancel.IsObject()) {
    stream_proto.object->Set("cancel", stream_cancel);
  }

  const Value locked_getter = interpreter_->NewNativeValue("locked", [](NativeCall& call) -> Value {
    if (!call.self.IsObject()) {
      return Value::Bool(false);
    }
    const Value* response_value = call.self.object->GetOwn(kStreamResponseSlot);
    if (response_value == nullptr || !response_value->IsObject()) {
      return Value::Bool(false);
    }
    const Value* locked = response_value->object->GetOwn(kBodyLockedSlot);
    return Value::Bool(locked != nullptr && js::ToBoolean(*locked));
  });
  if (locked_getter.IsObject()) {
    stream_proto.object->DefineAccessor("locked", locked_getter.object, nullptr);
  }

  const Value read = interpreter_->NewNativeValue("read", [](NativeCall& reader_call) -> Value {
    if (!reader_call.self.IsObject() ||
        reader_call.self.object->GetOwn(kReaderBrandSlot) == nullptr) {
      return SettledPromise(reader_call.interpreter,
                            reader_call.interpreter.MakeError("TypeError", "not a reader"), true);
    }
    const Value result = reader_call.interpreter.NewObjectValue();
    if (!result.IsObject()) {
      return SettledPromise(reader_call.interpreter, Value::Undefined(), false);
    }
    const Value* done_flag = reader_call.self.object->GetOwn(kReaderDoneSlot);
    if (done_flag != nullptr && js::ToBoolean(*done_flag)) {
      result.object->Set("done", Value::Bool(true));
      result.object->Set("value", Value::Undefined());
      return SettledPromise(reader_call.interpreter, result, false);
    }
    reader_call.self.object->SetHidden(kReaderDoneSlot, Value::Bool(true));

    const Value* owning_response = reader_call.self.object->GetOwn(kStreamResponseSlot);
    if (owning_response == nullptr || !owning_response->IsObject()) {
      return SettledPromise(reader_call.interpreter,
                            reader_call.interpreter.MakeError("TypeError", "not a reader"), true);
    }
    owning_response->object->SetHidden(kBodyUsedSlot, Value::Bool(true));
    const Value* body = owning_response->object->GetOwn(kBodySlot);
    const std::string bytes = body == nullptr ? std::string() : js::ToString(*body);
    if (bytes.empty()) {
      result.object->Set("done", Value::Bool(true));
      result.object->Set("value", Value::Undefined());
      return SettledPromise(reader_call.interpreter, result, false);
    }
    const Value chunk = BytesToUint8Array(reader_call.interpreter, bytes);
    if (!chunk.IsObject() || chunk.object->GetKind() != js::Object::Kind::TypedArray) {
      return SettledPromise(
          reader_call.interpreter,
          chunk.IsObject() ? chunk
                           : reader_call.interpreter.MakeError("TypeError",
                                                              "failed to build body chunk"),
          true);
    }
    result.object->Set("done", Value::Bool(false));
    result.object->Set("value", chunk);
    return SettledPromise(reader_call.interpreter, result, false);
  });
  if (read.IsObject()) {
    reader_proto.object->Set("read", read);
  }

  const Value reader_cancel = interpreter_->NewNativeValue(
      "cancel", [](NativeCall& reader_call) -> Value {
        if (!reader_call.self.IsObject() ||
            reader_call.self.object->GetOwn(kReaderBrandSlot) == nullptr) {
          return SettledPromise(
              reader_call.interpreter,
              reader_call.interpreter.MakeError("TypeError", "not a reader"), true);
        }
        reader_call.self.object->SetHidden(kReaderDoneSlot, Value::Bool(true));
        if (const Value* owning_response = reader_call.self.object->GetOwn(kStreamResponseSlot)) {
          if (owning_response->IsObject()) {
            owning_response->object->SetHidden(kBodyUsedSlot, Value::Bool(true));
            owning_response->object->SetHidden(kBodyLockedSlot, Value::Bool(false));
          }
        }
        return SettledPromise(reader_call.interpreter, Value::Undefined(), false);
      });
  if (reader_cancel.IsObject()) {
    reader_proto.object->Set("cancel", reader_cancel);
  }

  const Value release = interpreter_->NewNativeValue(
      "releaseLock", [](NativeCall& reader_call) -> Value {
        if (!reader_call.self.IsObject() ||
            reader_call.self.object->GetOwn(kReaderBrandSlot) == nullptr) {
          return reader_call.Throw("TypeError", "not a reader");
        }
        if (const Value* owning_response = reader_call.self.object->GetOwn(kStreamResponseSlot)) {
          if (owning_response->IsObject()) {
            owning_response->object->SetHidden(kBodyLockedSlot, Value::Bool(false));
          }
        }
        reader_call.self.object->SetHidden(kStreamResponseSlot, Value::Undefined());
        return Value::Undefined();
      });
  if (release.IsObject()) {
    reader_proto.object->Set("releaseLock", release);
  }

  const Value stream_ctor = interpreter_->NewNativeValue(
      "ReadableStream", [](NativeCall& call) -> Value {
        return call.Throw("TypeError", "Illegal constructor");
      });
  const Value reader_ctor = interpreter_->NewNativeValue(
      "ReadableStreamDefaultReader", [](NativeCall& call) -> Value {
        return call.Throw("TypeError", "Illegal constructor");
      });
  if (!stream_ctor.IsObject() || !reader_ctor.IsObject()) {
    return;
  }
  stream_ctor.object->Set("prototype", stream_proto);
  stream_proto.object->SetHidden("constructor", stream_ctor);
  reader_ctor.object->Set("prototype", reader_proto);
  reader_proto.object->SetHidden("constructor", reader_ctor);

  interpreter_->Global()->Set("ReadableStream", stream_ctor);
  interpreter_->GlobalScope()->Declare("ReadableStream", stream_ctor, false);
  interpreter_->Global()->Set("ReadableStreamDefaultReader", reader_ctor);
  interpreter_->GlobalScope()->Declare("ReadableStreamDefaultReader", reader_ctor, false);

  if (interfaces_.IsObject()) {
    interfaces_.object->Set("ReadableStream", stream_proto);
    interfaces_.object->Set("ReadableStreamDefaultReader", reader_proto);
  }
}

}  // namespace microbrowser::bindings
