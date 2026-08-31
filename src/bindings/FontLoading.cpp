#include "bindings/FontLoading.h"

#include <string>
#include <utility>

namespace microbrowser::bindings {

namespace {

using js::Value;

// On the document object: the FontFaceSet, made once. On the set: its `ready` promise, also made
// once -- the specification says `ready` is the *same* promise every time it is read, and a page
// that stores it before load and awaits it after has to see the one that settles.
constexpr const char* kFontFaceSetSlot = "#fontFaceSet";
constexpr const char* kReadySlot = "#fontsReady";
// Whether this document's fonts have settled. Written on the *document* by `SettleFontsReady` and
// copied onto the set when one is made, so the two orders both work: a set made before load is
// marked by the settle, and a set made after load is born marked. It is stored rather than inferred
// from the promise, because there is no way to ask a promise object whether it is still pending.
constexpr const char* kSettledSlot = "#fontsSettled";

bool AlreadySettled(const js::Object& object) {
  const Value* settled = object.GetOwn(kSettledSlot);
  return settled != nullptr && settled->type == js::ValueType::Boolean && settled->boolean;
}

// The promise `ready` answers with, made on first read. Resolved immediately when the document's
// fonts have already settled, which is the case for every read from an `onload` handler.
js::Object* ReadyPromise(js::Interpreter& interpreter, js::Object& set) {
  if (const Value* existing = set.GetOwn(kReadySlot);
      existing != nullptr && existing->IsObject()) {
    return existing->object;
  }
  const Value promise = interpreter.NewPromiseValue();
  if (!promise.IsObject()) {
    return nullptr;
  }
  set.Set(kReadySlot, promise);
  if (AlreadySettled(set)) {
    interpreter.SettleAsyncResult(promise.object, Value::Obj(&set), false);
  }
  return promise.object;
}

}  // namespace

js::Object* FontFaceSetFor(js::Interpreter& interpreter, js::Object& document) {
  if (const Value* existing = document.GetOwn(kFontFaceSetSlot);
      existing != nullptr && existing->IsObject()) {
    return existing->object;
  }
  const Value made = interpreter.NewObjectValue();
  if (!made.IsObject()) {
    return nullptr;
  }
  document.Set(kFontFaceSetSlot, made);
  // Born settled when the document's fonts already are -- which is every read from an `onload`
  // handler, since the load event is what settles them.
  made.object->Set(kSettledSlot, Value::Bool(AlreadySettled(document)));

  // `status`, and it is an accessor rather than a stored string so that it cannot fall out of step
  // with the promise: both read the one settled flag.
  const Value status = interpreter.NewNativeValue("status", [](js::NativeCall& call) {
    const js::Object* self = call.self.IsObject() ? call.self.object : nullptr;
    const bool loaded = self != nullptr && AlreadySettled(*self);
    return Value::String(std::string(loaded ? "loaded" : "loading"));
  });
  if (status.IsObject()) {
    made.object->DefineAccessor("status", status.object, nullptr);
  }

  const Value ready = interpreter.NewNativeValue("ready", [](js::NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    js::Object* promise = ReadyPromise(call.interpreter, *call.self.object);
    return promise == nullptr ? Value::Undefined() : Value::Obj(promise);
  });
  if (ready.IsObject()) {
    made.object->DefineAccessor("ready", ready.object, nullptr);
  }
  return made.object;
}

void SettleFontsReady(js::Interpreter& interpreter, js::Object& document) {
  // Only when the page has actually asked for `document.fonts`. A document whose script never
  // mentioned it should not grow a set and a promise at load, which is the same reasoning that
  // keeps `requestAnimationFrame` from scheduling a frame nobody asked for.
  document.Set(kSettledSlot, Value::Bool(true));
  const Value* existing = document.GetOwn(kFontFaceSetSlot);
  if (existing == nullptr || !existing->IsObject()) {
    return;  // nothing read `document.fonts`, so there is no set and no promise to settle
  }
  js::Object& set = *existing->object;
  if (AlreadySettled(set)) {
    return;
  }
  set.Set(kSettledSlot, Value::Bool(true));
  // Only settle a promise that exists. Making one here would be making one nobody read.
  if (const Value* promise = set.GetOwn(kReadySlot);
      promise != nullptr && promise->IsObject()) {
    interpreter.SettleAsyncResult(promise->object, Value::Obj(&set), false);
  }
}

}  // namespace microbrowser::bindings
