// `Headers` and `Response`: the object model a page holds, apart from the act
// of fetching, which is in FetchBindings.cpp.
//
// **Nothing in this file decides who may read what.** What a `Response` is made
// of arrives from `src/engine` already filtered: an opaque response is empty
// because `net` threw the bytes away before this module existed in the call
// stack, not because a getter here refuses to return them (ADR 0020 §2). That
// is the difference between a check and a curtain, and it is why there is no
// same-origin comparison in this file at all.
//
// `response.body` is a `ReadableStream` that yields the buffered body as one
// chunk. That is not progressive delivery -- the bytes are already in hand --
// but it is the honest shape SABR and every `getReader()` consumer need, and
// ADR 0020's "absent rather than buffering" rule no longer holds once a
// target site requires the stream. A page that streams still sees one chunk
// rather than a lie that nothing arrived.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/FetchSupport.h"
#include "bindings/Network.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// Header names are compared folded and stored folded, which is what makes
// `headers.get('Content-Type')` and `headers.get('content-type')` the same
// question. The value keeps its case: only the name is case-insensitive.
std::string FoldName(const Value& value) { return LowerCase(js::ToString(value)); }

// A promise that is already settled. Body methods and the one-chunk reader
// both answer with one of these: the bytes are in hand, and a promise is the
// shape of the API rather than a claim that anything is still happening.
Value SettledPromise(js::Interpreter& interpreter, const Value& value, bool rejected) {
  const Value promise = interpreter.NewPromiseValue();
  if (promise.IsObject()) {
    interpreter.SettleAsyncResult(promise.object, value, rejected);
  }
  return promise;
}

// The body bytes as a `Uint8Array`, which is what `ReadableStreamDefaultReader`
// yields and what YouTube's SABR buffer (`qU.append`) indexes by `.buffer` /
// `.byteOffset` / `.length`.
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

Value MakeBodyStream(js::Interpreter& interpreter, const Value& response) {
  const Value stream = interpreter.NewObjectValue();
  if (!stream.IsObject() || !response.IsObject()) {
    return Value::Undefined();
  }
  stream.object->SetHidden(kStreamResponseSlot, response);

  const Value get_reader =
      interpreter.NewNativeValue("getReader", [](NativeCall& stream_call) -> Value {
        const Value* response_value = stream_call.self.IsObject()
                                          ? stream_call.self.object->GetOwn(kStreamResponseSlot)
                                          : nullptr;
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
        reader.object->SetHidden(kStreamResponseSlot, *response_value);
        reader.object->SetHidden(kReaderDoneSlot, Value::Bool(false));

        const Value read =
            stream_call.interpreter.NewNativeValue("read", [](NativeCall& reader_call) -> Value {
              if (!reader_call.self.IsObject() ||
                  reader_call.self.object->GetOwn(kStreamResponseSlot) == nullptr) {
                return SettledPromise(
                    reader_call.interpreter,
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

              const Value* owning_response =
                  reader_call.self.object->GetOwn(kStreamResponseSlot);
              if (owning_response == nullptr || !owning_response->IsObject()) {
                return SettledPromise(
                    reader_call.interpreter,
                    reader_call.interpreter.MakeError("TypeError", "not a reader"), true);
              }
              owning_response->object->SetHidden(kBodyUsedSlot, Value::Bool(true));
              const Value* body = owning_response->object->GetOwn(kBodySlot);
              const std::string bytes =
                  body == nullptr ? std::string() : js::ToString(*body);
              if (bytes.empty()) {
                result.object->Set("done", Value::Bool(true));
                result.object->Set("value", Value::Undefined());
                return SettledPromise(reader_call.interpreter, result, false);
              }
              const Value chunk = BytesToUint8Array(reader_call.interpreter, bytes);
              if (!chunk.IsObject() || chunk.object->GetKind() != js::Object::Kind::TypedArray) {
                return SettledPromise(
                    reader_call.interpreter,
                    chunk.IsObject()
                        ? chunk
                        : reader_call.interpreter.MakeError("TypeError",
                                                           "failed to build body chunk"),
                    true);
              }
              result.object->Set("done", Value::Bool(false));
              result.object->Set("value", chunk);
              return SettledPromise(reader_call.interpreter, result, false);
            });
        if (read.IsObject()) {
          reader.object->Set("read", read);
        }

        const Value cancel = stream_call.interpreter.NewNativeValue(
            "cancel", [](NativeCall& reader_call) -> Value {
              if (reader_call.self.IsObject()) {
                reader_call.self.object->SetHidden(kReaderDoneSlot, Value::Bool(true));
                if (const Value* owning_response =
                        reader_call.self.object->GetOwn(kStreamResponseSlot)) {
                  if (owning_response->IsObject()) {
                    owning_response->object->SetHidden(kBodyUsedSlot, Value::Bool(true));
                  }
                }
              }
              return SettledPromise(reader_call.interpreter, Value::Undefined(), false);
            });
        if (cancel.IsObject()) {
          reader.object->Set("cancel", cancel);
        }
        return reader;
      });
  if (get_reader.IsObject()) {
    stream.object->Set("getReader", get_reader);
  }
  return stream;
}

}  // namespace

// --- Headers ----------------------------------------------------------------

js::Value DomBindings::MakeHeaders(const std::vector<ScriptHeader>& fields) {
  const Value made = interpreter_->NewObjectValue();
  if (!made.IsObject()) {
    return Value::Undefined();
  }
  const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn("Headers") : nullptr;
  if (prototype != nullptr && prototype->IsObject()) {
    made.object->SetPrototype(prototype->object);
  }
  std::vector<Value> pairs;
  pairs.reserve(fields.size());
  for (const ScriptHeader& field : fields) {
    pairs.push_back(MakePair(*interpreter_, LowerCase(field.name), field.value));
  }
  WritePairs(*interpreter_, made, kHeaderPairsSlot, std::move(pairs));
  return made;
}

void DomBindings::InstallHeaders() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("Headers", prototype);

  const auto method = [this, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->Set(name, native);
    }
  };

  method("get", [](NativeCall& call) {
    const std::string wanted = FoldName(Argument(call.arguments, 0));
    // Every match, joined with ", ". A page reading `set-cookie` will not see
    // one -- `net` removed it -- but `Accept` legitimately repeats, and
    // returning only the first is the bug that makes a content negotiation
    // library ask for the wrong thing.
    std::string joined;
    bool found = false;
    for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
      if (PairPart(pair, 0) != wanted) {
        continue;
      }
      if (found) {
        joined += ", ";
      }
      joined += PairPart(pair, 1);
      found = true;
    }
    return found ? Value::String(std::move(joined)) : Value::Null();
  });
  method("has", [](NativeCall& call) {
    const std::string wanted = FoldName(Argument(call.arguments, 0));
    for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
      if (PairPart(pair, 0) == wanted) {
        return Value::Bool(true);
      }
    }
    return Value::Bool(false);
  });
  method("set", [](NativeCall& call) {
    const std::string name = FoldName(Argument(call.arguments, 0));
    const std::string value = js::ToString(Argument(call.arguments, 1));
    std::vector<Value> kept;
    bool written = false;
    for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
      if (PairPart(pair, 0) != name) {
        kept.push_back(pair);
        continue;
      }
      if (!written) {
        // In place, because `set` replaces the value and keeps the position:
        // a page that sets a header twice and then iterates must not see it
        // move to the end.
        kept.push_back(MakePair(call.interpreter, name, value));
        written = true;
      }
    }
    if (!written) {
      kept.push_back(MakePair(call.interpreter, name, value));
    }
    WritePairs(call.interpreter, call.self, kHeaderPairsSlot, std::move(kept));
    return Value::Undefined();
  });
  method("append", [](NativeCall& call) {
    const std::string name = FoldName(Argument(call.arguments, 0));
    std::vector<Value> pairs = ReadPairs(call.self, kHeaderPairsSlot);
    pairs.push_back(MakePair(call.interpreter, name,
                             js::ToString(Argument(call.arguments, 1))));
    WritePairs(call.interpreter, call.self, kHeaderPairsSlot, std::move(pairs));
    return Value::Undefined();
  });
  method("delete", [](NativeCall& call) {
    const std::string name = FoldName(Argument(call.arguments, 0));
    std::vector<Value> kept;
    for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
      if (PairPart(pair, 0) != name) {
        kept.push_back(pair);
      }
    }
    WritePairs(call.interpreter, call.self, kHeaderPairsSlot, std::move(kept));
    return Value::Undefined();
  });
  method("forEach", [](NativeCall& call) {
    const Value callback = Argument(call.arguments, 0);
    if (!callback.IsObject() || !callback.object->IsCallable()) {
      return call.Throw("TypeError", "Headers.forEach requires a function");
    }
    for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
      // Value, name, this -- the order every headers implementation uses and
      // the reverse of what reading the pair suggests.
      const js::Result ran = call.interpreter.CallFunction(
          callback, Argument(call.arguments, 1),
          {Value::String(PairPart(pair, 1)), Value::String(PairPart(pair, 0)), call.self});
      if (ran.IsAbrupt()) {
        return call.ThrowValue(ran.value);
      }
    }
    return Value::Undefined();
  });

  // `entries`, `keys`, `values` and `for (const [k, v] of headers)`. All four
  // are one list built four ways, handed to the array iterator -- which is
  // taken from the interpreter rather than from the global, because a page can
  // reassign `Symbol.iterator` and the protocol must not follow it.
  const auto iterator = [this](int part) {
    return [part](NativeCall& call) {
      std::vector<Value> out;
      for (const Value& pair : ReadPairs(call.self, kHeaderPairsSlot)) {
        if (part == 0) {
          out.push_back(Value::String(PairPart(pair, 0)));
        } else if (part == 1) {
          out.push_back(Value::String(PairPart(pair, 1)));
        } else {
          out.push_back(call.interpreter.NewArrayValue(
              {Value::String(PairPart(pair, 0)), Value::String(PairPart(pair, 1))}));
        }
      }
      const Value entries = call.interpreter.NewArrayValue(std::move(out));
      if (!entries.IsObject()) {
        return Value::Undefined();
      }
      const Value* protocol =
          entries.object->Get(js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
      if (protocol == nullptr) {
        return Value::Undefined();
      }
      const js::Result made = call.interpreter.CallFunction(*protocol, entries, {});
      return made.IsAbrupt() ? Value::Undefined() : made.value;
    };
  };
  method("keys", iterator(0));
  method("values", iterator(1));
  method("entries", iterator(2));
  const Value iterate = interpreter_->NewNativeValue("[Symbol.iterator]", iterator(2));
  if (iterate.IsObject()) {
    prototype.object->Set(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), iterate);
  }

  const Value constructor =
      interpreter_->NewNativeValue("Headers", [prototype](NativeCall& call) {
        const Value made = call.interpreter.NewObjectValue();
        if (!made.IsObject()) {
          return Value::Undefined();
        }
        made.object->SetPrototype(prototype.object);
        WritePairs(call.interpreter, made, kHeaderPairsSlot, {});
        const Value init = Argument(call.arguments, 0);
        if (init.IsObject()) {
          const Value* existing = init.object->GetOwn(kHeaderPairsSlot);
          if (existing != nullptr) {
            // Copied rather than shared: `new Headers(other)` is a snapshot,
            // and aliasing the array would make a write to one show up in both.
            std::vector<Value> copy;
            for (const Value& pair : ReadPairs(init, kHeaderPairsSlot)) {
              copy.push_back(
                  MakePair(call.interpreter, PairPart(pair, 0), PairPart(pair, 1)));
            }
            WritePairs(call.interpreter, made, kHeaderPairsSlot, std::move(copy));
          } else if (init.object->ElementCount() > 0) {
            std::vector<Value> copy;
            for (std::size_t i = 0; i < init.object->ElementCount(); ++i) {
              const Value entry = init.object->GetElement(i);
              copy.push_back(MakePair(call.interpreter, LowerCase(PairPart(entry, 0)),
                                      PairPart(entry, 1)));
            }
            WritePairs(call.interpreter, made, kHeaderPairsSlot, std::move(copy));
          } else {
            std::vector<Value> copy;
            for (const std::string& key : init.object->EnumerableKeys()) {
              const Value* value = init.object->Get(key);
              copy.push_back(MakePair(call.interpreter, LowerCase(key),
                                      value == nullptr ? std::string() : js::ToString(*value)));
            }
            WritePairs(call.interpreter, made, kHeaderPairsSlot, std::move(copy));
          }
        }
        return made;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->SetHidden("constructor", constructor);
    interpreter_->Global()->Set("Headers", constructor);
    interpreter_->GlobalScope()->Declare("Headers", constructor, false);
  }
}

// --- Response ---------------------------------------------------------------

js::Value DomBindings::MakeResponse(const ScriptResponse& response) {
  const Value made = interpreter_->NewObjectValue();
  if (!made.IsObject()) {
    return Value::Undefined();
  }
  const Value* prototype =
      interfaces_.IsObject() ? interfaces_.object->GetOwn("Response") : nullptr;
  if (prototype != nullptr && prototype->IsObject()) {
    made.object->SetPrototype(prototype->object);
  }
  made.object->Set("status", Value::Number(response.status));
  made.object->Set("statusText", Value::String(response.status_text));
  // `ok` is about the status and nothing else. A 404 that arrived is not `ok`
  // and did not fail; a page that treats every settled fetch as success is the
  // most common bug in this API and the reason the two are separate.
  made.object->Set("ok", Value::Bool(response.status >= 200 && response.status <= 299));
  made.object->Set("url", Value::String(response.url));
  made.object->Set("redirected", Value::Bool(response.redirected));
  made.object->Set("type", Value::String(response.opaque ? "opaque" : "basic"));
  made.object->Set("headers", MakeHeaders(response.headers));
  made.object->SetHidden(kBodySlot, Value::String(response.body));
  made.object->SetHidden(kBodyUsedSlot, Value::Bool(false));
  made.object->SetHidden(kBodyLockedSlot, Value::Bool(false));
  return made;
}

void DomBindings::InstallResponse() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("Response", prototype);

  const auto method = [this, &prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      prototype.object->Set(name, native);
    }
  };

  // A body may be read once. The flag is set before the value is produced, so
  // the second `text()` on the same response rejects rather than handing out a
  // buffer a stream would already have consumed -- which is the behaviour a
  // page written against a streaming implementation depends on. A locked
  // stream (a reader outstanding) is the same refusal: the bytes have a single
  // consumer.
  const auto take_body = [](NativeCall& call, std::string& out) -> bool {
    if (!HasSlot(call.self, kBodySlot)) {
      call.Throw("TypeError", "not a Response");
      return false;
    }
    const Value* locked = call.self.object->GetOwn(kBodyLockedSlot);
    if (locked != nullptr && js::ToBoolean(*locked)) {
      call.Throw("TypeError", "body is locked");
      return false;
    }
    const Value* used = call.self.object->GetOwn(kBodyUsedSlot);
    if (used != nullptr && js::ToBoolean(*used)) {
      call.Throw("TypeError", "body already read");
      return false;
    }
    call.self.object->SetHidden(kBodyUsedSlot, Value::Bool(true));
    const Value* body = call.self.object->GetOwn(kBodySlot);
    out = body == nullptr ? std::string() : js::ToString(*body);
    return true;
  };

  method("text", [take_body](NativeCall& call) {
    std::string body;
    if (!take_body(call, body)) {
      return SettledPromise(call.interpreter, call.ThrownValue(), true);
    }
    return SettledPromise(call.interpreter, Value::String(std::move(body)), false);
  });
  method("json", [take_body](NativeCall& call) {
    std::string body;
    if (!take_body(call, body)) {
      return SettledPromise(call.interpreter, call.ThrownValue(), true);
    }
    // The page's own `JSON.parse`, not a second parser. A browser with two
    // JSON implementations has two answers for a duplicate key, and the one
    // reached through `fetch` would be the one nobody tested.
    //
    // From the global *scope* rather than the global object, which is where
    // `src/js` declares its builtins: a language global is a binding, and only
    // the things this module installs are properties of `window`.
    const Value* json = call.interpreter.GlobalScope()->Lookup("JSON");
    const Value* parse =
        json != nullptr && json->IsObject() ? json->object->Get("parse") : nullptr;
    if (parse == nullptr) {
      return SettledPromise(call.interpreter,
                            call.interpreter.MakeError("TypeError", "JSON.parse is unavailable"),
                            true);
    }
    const js::Result parsed =
        call.interpreter.CallFunction(*parse, Value::Undefined(), {Value::String(body)});
    return SettledPromise(call.interpreter, parsed.value, parsed.IsAbrupt());
  });
  method("arrayBuffer", [take_body](NativeCall& call) {
    std::string body;
    if (!take_body(call, body)) {
      return SettledPromise(call.interpreter, call.ThrownValue(), true);
    }
    const Value* constructor = call.interpreter.GlobalScope()->Lookup("ArrayBuffer");
    if (constructor == nullptr) {
      return SettledPromise(
          call.interpreter,
          call.interpreter.MakeError("TypeError", "ArrayBuffer is unavailable"), true);
    }
    const js::Result buffer = call.interpreter.ConstructValue(
        *constructor, {Value::Number(static_cast<double>(body.size()))});
    if (buffer.IsAbrupt() || !buffer.value.IsObject()) {
      return SettledPromise(call.interpreter, buffer.value, true);
    }
    // Copied straight into the buffer's bytes rather than through a typed array
    // one element at a time: a megabyte of response would otherwise be a
    // million property writes through the interpreter.
    const js::BufferView* view = buffer.value.object->View();
    if (view != nullptr && view->bytes != nullptr && view->bytes->size() >= body.size()) {
      std::copy(body.begin(), body.end(), view->bytes->begin());
    }
    return SettledPromise(call.interpreter, buffer.value, false);
  });
  method("clone", [](NativeCall& call) {
    if (!HasSlot(call.self, kBodySlot)) {
      return call.Throw("TypeError", "not a Response");
    }
    const Value* used = call.self.object->GetOwn(kBodyUsedSlot);
    if (used != nullptr && js::ToBoolean(*used)) {
      return call.Throw("TypeError", "body already read");
    }
    const Value* locked = call.self.object->GetOwn(kBodyLockedSlot);
    if (locked != nullptr && js::ToBoolean(*locked)) {
      return call.Throw("TypeError", "body is locked");
    }
    const Value made = call.interpreter.NewObjectValue();
    if (!made.IsObject()) {
      return Value::Undefined();
    }
    made.object->SetPrototype(call.self.object->Prototype());
    for (const std::string& key : call.self.object->EnumerableKeys()) {
      if (const Value* value = call.self.object->Get(key)) {
        made.object->Set(key, *value);
      }
    }
    const Value* body = call.self.object->GetOwn(kBodySlot);
    made.object->SetHidden(kBodySlot, body == nullptr ? Value::String("") : *body);
    made.object->SetHidden(kBodyUsedSlot, Value::Bool(false));
    made.object->SetHidden(kBodyLockedSlot, Value::Bool(false));
    return made;
  });

  const Value used = interpreter_->NewNativeValue("bodyUsed", [](NativeCall& call) {
    const Value* flag =
        call.self.IsObject() ? call.self.object->GetOwn(kBodyUsedSlot) : nullptr;
    return Value::Bool(flag != nullptr && js::ToBoolean(*flag));
  });
  if (used.IsObject()) {
    prototype.object->DefineAccessor("bodyUsed", used.object, nullptr);
  }

  // One-chunk stream over the buffered body. Opaque responses keep `null`: the
  // bytes were discarded in `net`, and a stream here would invent an empty
  // body a page could mistake for a successful read of cross-origin content.
  const Value body_getter = interpreter_->NewNativeValue("body", [](NativeCall& call) -> Value {
    if (!HasSlot(call.self, kBodySlot)) {
      return Value::Undefined();
    }
    if (const Value* type = call.self.object->Get("type")) {
      if (js::ToString(*type) == "opaque") {
        return Value::Null();
      }
    }
    if (const Value* cached = call.self.object->GetOwn(kBodyStreamSlot)) {
      if (cached->IsObject()) {
        return *cached;
      }
    }
    const Value stream = MakeBodyStream(call.interpreter, call.self);
    if (stream.IsObject()) {
      call.self.object->SetHidden(kBodyStreamSlot, stream);
    }
    return stream;
  });
  if (body_getter.IsObject()) {
    prototype.object->DefineAccessor("body", body_getter.object, nullptr);
  }

  // Feature detection for innertube / SABR: `window.ReadableStream && ...`.
  // Construction is refused -- pages that `new ReadableStream({start})` need
  // the full controller model, and inventing a half of it is ADR 0012's stub.
  if (interpreter_->GlobalScope()->Lookup("ReadableStream") == nullptr) {
    const Value stream_ctor = interpreter_->NewNativeValue(
        "ReadableStream", [](NativeCall& call) -> Value {
          return call.Throw("TypeError", "Illegal constructor");
        });
    if (stream_ctor.IsObject()) {
      interpreter_->Global()->Set("ReadableStream", stream_ctor);
      interpreter_->GlobalScope()->Declare("ReadableStream", stream_ctor, false);
    }
  }

  // `new Response(body, init)`, which a page uses to hand a synthesised answer
  // to something that expects one -- a service worker's shape, in a browser
  // with no service workers, and cheap enough to have anyway.
  const Value constructor =
      interpreter_->NewNativeValue("Response", [this, prototype](NativeCall& call) {
        ScriptResponse made;
        made.ok = true;
        made.status = 200;
        made.status_text = "OK";
        const Value body = Argument(call.arguments, 0);
        if (!body.IsUndefined() && !body.IsNull()) {
          made.body = js::ToString(body);
        }
        const Value init = Argument(call.arguments, 1);
        if (init.IsObject()) {
          if (const Value* status = init.object->Get("status")) {
            made.status = static_cast<int>(js::ToNumber(*status));
          }
          if (const Value* text = init.object->Get("statusText")) {
            made.status_text = js::ToString(*text);
          }
        }
        Value response = MakeResponse(made);
        if (response.IsObject() && prototype.IsObject()) {
          response.object->SetPrototype(prototype.object);
        }
        return response;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->SetHidden("constructor", constructor);
    interpreter_->Global()->Set("Response", constructor);
    interpreter_->GlobalScope()->Declare("Response", constructor, false);
  }
}

// --- Request ----------------------------------------------------------------

void DomBindings::InstallRequest() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("Request", prototype);

  // Deliberately thin. A `Request` here is the *arguments* to a fetch as a
  // value a page can pass around and clone -- which is what a router or an
  // interceptor uses one for -- and not a second request path. `fetch` reads
  // `url`, `method` and `headers` off whatever it is handed, so a page's own
  // object with those three properties works exactly as well, and there is no
  // branch anywhere that asks whether the thing it was given is really one of
  // these. Anything more would be a second place that decides what a request
  // is.
  const Value constructor =
      interpreter_->NewNativeValue("Request", [this, prototype](NativeCall& call) {
        const Value made = call.interpreter.NewObjectValue();
        if (!made.IsObject()) {
          return Value::Undefined();
        }
        made.object->SetPrototype(prototype.object);
        const Value input = Argument(call.arguments, 0);
        std::string url;
        std::string method = "GET";
        std::string mode = "cors";
        std::string credentials = "same-origin";
        std::vector<ScriptHeader> headers;
        std::string body_bytes;
        bool body_from_string = false;
        Value signal;
        if (input.IsObject() && input.object->GetOwn("url") != nullptr) {
          url = js::ToString(*input.object->Get("url"));
          if (const Value* existing = input.object->Get("method")) {
            method = js::ToString(*existing);
          }
          if (const Value* existing = input.object->Get("mode")) {
            mode = js::ToString(*existing);
          }
          if (const Value* existing = input.object->Get("credentials")) {
            credentials = js::ToString(*existing);
          }
          if (const Value* existing = input.object->Get("headers")) {
            for (const Value& pair : ReadPairs(*existing, kHeaderPairsSlot)) {
              headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
            }
          }
          if (const Value* existing = input.object->GetOwn(kRequestBodySlot)) {
            body_bytes =
                existing->IsString() ? existing->AsString() : js::ToString(*existing);
            const Value* from_string = input.object->GetOwn(kRequestBodyFromStringSlot);
            body_from_string = from_string != nullptr && js::ToBoolean(*from_string);
          }
          if (const Value* existing = input.object->GetOwn(kRequestSignalSlot)) {
            signal = *existing;
          }
        } else {
          url = js::ToString(input);
        }
        const Value init = Argument(call.arguments, 1);
        if (init.IsObject()) {
          if (const Value* given = init.object->Get("method")) {
            method = js::ToString(*given);
          }
          if (const Value* given = init.object->Get("body")) {
            if (!given->IsUndefined() && !given->IsNull()) {
              if (!ExtractRequestBody(*given, body_bytes, body_from_string)) {
                return call.Throw("TypeError", "failed to read request body");
              }
            }
          }
          if (const Value* given = init.object->Get("mode")) {
            mode = js::ToString(*given);
          }
          if (const Value* given = init.object->Get("credentials")) {
            credentials = js::ToString(*given);
          }
          if (const Value* given = init.object->Get("headers")) {
            if (given->IsObject() && given->object->GetOwn(kHeaderPairsSlot) != nullptr) {
              headers.clear();
              for (const Value& pair : ReadPairs(*given, kHeaderPairsSlot)) {
                headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
              }
            } else if (given->IsObject()) {
              headers.clear();
              for (const std::string& key : given->object->EnumerableKeys()) {
                const Value* value = given->object->Get(key);
                headers.push_back(ScriptHeader{
                    LowerCase(key), value == nullptr ? std::string() : js::ToString(*value)});
              }
            }
          }
          if (const Value* given = init.object->Get("signal")) {
            signal = *given;
          }
        }
        made.object->Set("url", Value::String(std::move(url)));
        made.object->Set("method", Value::String(std::move(method)));
        made.object->Set("mode", Value::String(std::move(mode)));
        made.object->Set("credentials", Value::String(std::move(credentials)));
        made.object->Set("headers", MakeHeaders(headers));
        if (!body_bytes.empty() || body_from_string) {
          made.object->SetHidden(kRequestBodySlot, Value::String(std::move(body_bytes)));
          made.object->SetHidden(kRequestBodyFromStringSlot, Value::Bool(body_from_string));
        }
        if (signal.IsObject()) {
          made.object->SetHidden(kRequestSignalSlot, signal);
        }
        return made;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->SetHidden("constructor", constructor);
    interpreter_->Global()->Set("Request", constructor);
    interpreter_->GlobalScope()->Declare("Request", constructor, false);
  }
}

}  // namespace microbrowser::bindings
