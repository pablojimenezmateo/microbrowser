// `Worker`, `structuredClone`, and nothing else from ADR 0022.
//
// Session 38. `SharedWorker`, `navigator.serviceWorker` and the Cache API are **absent**, and each
// absence is the ADR's decision rather than an omission: shared mutable state across documents is the
// one thing the worker model exists to avoid, and a service worker is a script that runs when no page
// is open, which this browser will not do. Under ADR 0012's rule a page that feature-detects them takes
// its no-worker path, which for a service worker means "work online", and that is the correct outcome.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Workers.h"
#include "js/Interpreter.h"
#include "js/StructuredClone.h"
#include "js/Value.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

constexpr const char* kWorkerIdSlot = "#worker-id";

std::uint64_t WorkerIdOf(const Value& object) {
  if (!object.IsObject()) {
    return 0;
  }
  const Value* value = object.object->GetOwn(kWorkerIdSlot);
  if (value == nullptr || !value->IsNumber() || value->number <= 0.0) {
    return 0;
  }
  return static_cast<std::uint64_t>(value->number);
}

// Serialized bytes as a `std::string`, which is what crosses the seam.
//
// A string rather than a `std::vector<std::uint8_t>` for one reason: the seam is declared in this module
// and `js::SerializedValue` is `src/js`'s type, which this module may see but `src/engine`'s callers
// would then have to agree about. A string of bytes is the narrowest thing both sides already share --
// and it is opaque to both, which is the point.
std::string BytesOf(const js::SerializedValue& serialized) {
  return std::string(reinterpret_cast<const char*>(serialized.bytes.data()),
                     serialized.bytes.size());
}

}  // namespace

DomBindings::DomBindings(js::Interpreter& interpreter, std::string url, NetworkSource* network)
    : interpreter_(&interpreter),
      document_(nullptr),
      url_(std::move(url)),
      network_(network) {}

void DomBindings::InstallWorkerScope() {
  if (interpreter_ == nullptr) {
    return;
  }
  // **The list is the decision.** Each of these is exposed in a `WorkerGlobalScope` by the
  // specification and is already implemented here without ever asking about a document -- which is
  // what makes reusing them right rather than expedient: `new URL('a', b)` in a worker and on a page
  // are the same parser, so they cannot come to disagree.
  //
  // What is *not* here is as deliberate. `IndexedDB` and `localStorage` want a store keyed by an
  // origin and reached from the main thread; each is absent rather than present-and-broken, because
  // under ADR 0012's rule a script that finds a name and gets something that never answers has no
  // fallback left.
  InstallStructuredClone();
  InstallUrlConstructor();
  InstallUrlSearchParams();
  InstallTextEncoding();
  InstallCrypto();
  InstallBlob();
  // Same objects a page holds, and a worker script that feature-detects
  // `MessageChannel` and finds nothing builds its own scheduler on
  // `setTimeout`. Delivery goes through `QueueTask` when that queue exists and
  // `setTimeout(0)` when it does not -- workers have the latter.
  InstallMessageChannel();
  // `fetch`, `Headers`, `Request`, `Response`, `FormData`, `AbortController` and
  // `XMLHttpRequest` -- **the same code the page runs**, over a `NetworkSource` whose other end is
  // this worker's own thread. It installs nothing at all when there is no network behind it, which
  // is the same absence-rather-than-stub rule it follows on a page.
  InstallFetch();
}

void DomBindings::InstallStructuredClone() {
  if (interpreter_ == nullptr) {
    return;
  }
  // `structuredClone()`, which is the algorithm the worker seam already uses exposed as a function.
  // Worth having on its own: it is how a page deep-copies a value without `JSON.parse(JSON.stringify())`
  // -- which loses Maps, Dates, cycles and typed arrays, all of which this keeps.
  const Value clone = interpreter_->NewNativeValue("structuredClone", [](NativeCall& call) -> Value {
    const std::optional<js::SerializedValue> serialized =
        js::StructuredSerialize(call.interpreter, Argument(call.arguments, 0));
    if (!serialized.has_value()) {
      // `DataCloneError`, and throwing is the whole point: a clone that silently dropped a function
      // would hand a page an object that is *nearly* the one it asked for, and the bug surfaces later
      // in code that has no idea a clone happened.
      return ThrowDom(call, "DataCloneError", "the value could not be cloned");
    }
    return js::StructuredDeserialize(call.interpreter, *serialized);
  });
  if (clone.IsObject()) {
    interpreter_->Global()->Set("structuredClone", clone);
    interpreter_->GlobalScope()->Declare("structuredClone", clone, false);
  }
}

void DomBindings::InstallWorker() {
  EnsureInterfaces();
  if (workers_ == nullptr || !interfaces_.IsObject()) {
    // No host behind the layer, so `Worker` is absent rather than present-and-broken. A page that finds
    // `Worker` and gets one whose `onmessage` never fires has no fallback left; one that finds nothing
    // runs its work on the main thread, which is slower and correct.
    return;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("Worker", prototype);

  const Value post = interpreter_->NewNativeValue("postMessage", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t id = WorkerIdOf(call.self);
    if (owner == nullptr || owner->workers_ == nullptr || id == 0) {
      return Value::Undefined();
    }
    // **Serialised here, on the main thread**, because serialising walks the *page's* heap and only this
    // thread may. What crosses is bytes.
    const std::optional<js::SerializedValue> serialized =
        js::StructuredSerialize(call.interpreter, Argument(call.arguments, 0));
    if (!serialized.has_value()) {
      return ThrowDom(call, "DataCloneError", "the message could not be cloned");
    }
    owner->workers_->PostToWorker(id, BytesOf(*serialized));
    return Value::Undefined();
  });
  if (post.IsObject()) {
    post.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("postMessage", post);
  }

  const Value terminate = interpreter_->NewNativeValue("terminate", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    const std::uint64_t id = WorkerIdOf(call.self);
    if (owner != nullptr && owner->workers_ != nullptr && id != 0) {
      owner->workers_->TerminateWorker(id);
      // The id is cleared, so a later `postMessage` on the same object is a no-op rather than a post to
      // whatever now holds that number. Ids are not reused, but clearing it is one fewer thing to rely
      // on -- and it is what makes `terminate()` twice harmless.
      if (call.self.IsObject()) {
        call.self.object->SetHidden(kWorkerIdSlot, Value::Number(0.0));
      }
    }
    return Value::Undefined();
  });
  if (terminate.IsObject()) {
    terminate.object->Set(kOwnerSlot, OwnerValue(this));
    prototype.object->Set("terminate", terminate);
  }

  const Value constructor =
      interpreter_->NewNativeValue("Worker", [prototype](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->workers_ == nullptr) {
          return Value::Undefined();
        }
        const std::string url = js::ToString(Argument(call.arguments, 0));
        const Value worker = call.interpreter.NewObjectValue();
        if (!worker.IsObject()) {
          return Value::Undefined();
        }
        worker.object->SetPrototype(prototype.object);
        owner->InstallEventMethods(worker);
        // The id may be zero -- an unavailable script, a refused URL, the sixteen-worker limit -- and
        // the object is returned either way. `new Worker` is specified not to throw for a script that
        // fails to load: the failure arrives as an `error` event, which is what a page listens for.
        const std::uint64_t id = owner->workers_->StartWorker(url);
        worker.object->SetHidden(kWorkerIdSlot, Value::Number(static_cast<double>(id)));
        owner->RememberWorker(id, worker);
        return worker;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
    // Reachable from the constructor as well as installed on each instance:
    // `worker instanceof Worker` and `Worker.prototype.postMessage` are both
    // things a page writes, and both read this property. See the note in
    // XhrBindings.cpp, where the same omission cost youtube a transport.
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    interpreter_->Global()->Set("Worker", constructor);
    interpreter_->GlobalScope()->Declare("Worker", constructor, false);
  }
}

void DomBindings::RememberWorker(std::uint64_t id, const js::Value& worker) {
  EnsureInterfaces();
  if (id == 0 || !interfaces_.IsObject() || !worker.IsObject()) {
    return;
  }
  // Hung off the interfaces object, which the collector walks. A C++ map of `js::Value` would be
  // invisible to it, and a `Worker` collected while its thread was still running is a message delivered
  // to a freed object.
  Value table = Value::Undefined();
  if (const Value* existing = interfaces_.object->GetOwn("workers")) {
    table = *existing;
  }
  if (!table.IsObject()) {
    table = interpreter_->NewObjectValue();
    if (!table.IsObject()) {
      return;
    }
    interfaces_.object->Set("workers", table);
  }
  table.object->Set(std::to_string(id), worker);
}

bool DomBindings::DeliverWorkerMessage(std::uint64_t id, WorkerDelivery kind,
                                       const std::string& serialized, const std::string& text) {
  if (interpreter_ == nullptr || !interfaces_.IsObject()) {
    return false;
  }
  if (kind == WorkerDelivery::Console) {
    // A worker's console line, put on the page's console rather than dispatched. Without this a
    // worker's diagnostics land in an interpreter nobody ever reads, which is what made every bug in
    // this file cost a rebuild to find.
    interpreter_->LogConsoleLine("[worker] " + text);
    return false;
  }
  const Value* table = interfaces_.object->GetOwn("workers");
  if (table == nullptr || !table->IsObject()) {
    return false;
  }
  const Value* found = table->object->Get(std::to_string(id));
  if (found == nullptr || !found->IsObject()) {
    return false;
  }
  const Value worker = *found;
  const bool is_error = kind == WorkerDelivery::Error;
  const Value event = interpreter_->NewObjectValue();
  if (!event.IsObject()) {
    return false;
  }
  event.object->Set("type", Value::String(is_error ? "error" : "message"));
  event.object->Set("target", worker);
  if (is_error) {
    // Text, not a serialised exception. An error object from another heap crossing the boundary would be
    // an object graph copied for a diagnostic, and `message` is what a page's handler reads.
    event.object->Set("message", Value::String(text));
    event.object->Set("filename", Value::String(""));
    event.object->Set("lineno", Value::Number(0.0));
  } else {
    // **Deserialised here**, on the main thread, into the page's heap -- the mirror of the serialise in
    // `postMessage`. The bytes were opaque the whole way across.
    js::SerializedValue bytes;
    bytes.bytes.assign(serialized.begin(), serialized.end());
    event.object->Set("data", js::StructuredDeserialize(*interpreter_, bytes));
  }
  // **`dispatchEvent` and nothing else.** Calling the `onmessage` property here as well delivered
  // every message *twice*: `RunListenersOn` reads `on<type>` off the target as an implicit listener,
  // which is the specification's rule for a handler attribute, so an explicit call before it is a
  // second delivery. It is invisible on a page that only counts side effects and fatal to a harness
  // that counts results -- every subtest a worker reported arrived as two.
  if (const Value* dispatch = worker.object->GetOwn("dispatchEvent");
      dispatch != nullptr && dispatch->IsObject()) {
    const js::Result outcome = interpreter_->CallFunction(*dispatch, worker, {event});
    if (outcome.completion == js::Completion::Throw) {
      interpreter_->ReportUncaught(outcome.value, "worker event listener");
    }
    return true;
  }
  return false;
}

}  // namespace microbrowser::bindings
