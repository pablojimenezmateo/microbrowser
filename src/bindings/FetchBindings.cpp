// `fetch`, `AbortController`, and the table of requests in flight.
//
// ADR 0020 §1: one request path, and `fetch` is the shape of it.
// `XMLHttpRequest` arrives later expressed in terms of these rather than beside
// them, which is the whole reason `Headers` and `Response` are types in
// FetchTypes.cpp rather than fields on a call.
//
// The request goes out through `NetworkSource`, which `src/engine` implements
// over `net::RequestQueue`, and every policy decision -- the privacy verdict,
// CORS, the preflight -- happens on that side. This file starts a request,
// remembers the promise, and settles it when the answer comes back.
//
// `fetch` is not declared at all when there is no `NetworkSource`. An absence
// rather than a stub, and this is the place ADR 0012's rule matters most: a
// page that finds `fetch` and gets a rejection has no fallback path left, where
// a page that finds nothing loads a polyfill that works.

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

}  // namespace

// --- AbortController and AbortSignal ----------------------------------------

void DomBindings::InstallAbortController() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value signal_prototype = interpreter_->NewObjectValue();
  if (!signal_prototype.IsObject()) {
    return;
  }
  interfaces_.object->Set("AbortSignal", signal_prototype);

  const Value aborted = interpreter_->NewNativeValue("aborted", [](NativeCall& call) {
    const Value* flag = call.self.IsObject() ? call.self.object->GetOwn(kAbortedSlot) : nullptr;
    return Value::Bool(flag != nullptr && js::ToBoolean(*flag));
  });
  if (aborted.IsObject()) {
    signal_prototype.object->DefineAccessor("aborted", aborted.object, nullptr);
  }
  const Value reason = interpreter_->NewNativeValue("reason", [](NativeCall& call) {
    const Value* value = call.self.IsObject() ? call.self.object->GetOwn(kAbortReasonSlot) : nullptr;
    return value == nullptr ? Value::Undefined() : *value;
  });
  if (reason.IsObject()) {
    signal_prototype.object->DefineAccessor("reason", reason.object, nullptr);
  }

  const Value constructor =
      interpreter_->NewNativeValue("AbortController", [this, signal_prototype](NativeCall& call) {
        const Value controller = call.interpreter.NewObjectValue();
        const Value signal = call.interpreter.NewObjectValue();
        if (!controller.IsObject() || !signal.IsObject()) {
          return Value::Undefined();
        }
        signal.object->SetPrototype(signal_prototype.object);
        signal.object->SetHidden(kAbortedSlot, Value::Bool(false));
        // An event target, so `signal.addEventListener('abort', …)` works --
        // which is how every library that composes signals listens for one.
        InstallEventMethods(signal);
        controller.object->Set("signal", signal);

        const Value abort = call.interpreter.NewNativeValue("abort", [this](NativeCall& inner) {
          const Value* signal_value =
              inner.self.IsObject() ? inner.self.object->GetOwn("signal") : nullptr;
          if (signal_value == nullptr) {
            return Value::Undefined();
          }
          AbortSignalled(*signal_value, Argument(inner.arguments, 0));
          return Value::Undefined();
        });
        if (abort.IsObject()) {
          abort.object->Set(kOwnerSlot, PointerValue(this));
          controller.object->Set("abort", abort);
        }
        return controller;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    interpreter_->Global()->Set("AbortController", constructor);
    interpreter_->GlobalScope()->Declare("AbortController", constructor, false);
  }
}

void DomBindings::AbortSignalled(const js::Value& signal, const js::Value& reason) {
  if (interpreter_ == nullptr || !signal.IsObject()) {
    return;
  }
  const Value* already = signal.object->GetOwn(kAbortedSlot);
  if (already != nullptr && js::ToBoolean(*already)) {
    // Abort is idempotent. A controller aborted twice fires once, or a page
    // that calls it in both a timeout and a cleanup would reject a fetch that
    // had already settled.
    return;
  }
  Value why = reason;
  if (why.IsUndefined()) {
    // The name is what a page tests: `catch (e) { if (e.name !== 'AbortError') … }`
    // is how every cancellable request tells a cancellation from a failure.
    why = interpreter_->MakeError("Error", "The operation was aborted");
    if (why.IsObject()) {
      why.object->Set("name", Value::String("AbortError"));
    }
  }
  signal.object->SetHidden(kAbortedSlot, Value::Bool(true));
  signal.object->SetHidden(kAbortReasonSlot, why);

  // Every fetch this signal was passed to, rejected and cancelled at the
  // network. Walked rather than indexed: a page has a handful of requests in
  // flight, and a two-way link between a signal and a request would be a
  // reference this module has to keep correct across a collection.
  const Value* pending =
      interfaces_.IsObject() ? interfaces_.object->GetOwn(kPendingFetchSlot) : nullptr;
  if (pending != nullptr && pending->IsObject()) {
    std::vector<Value> kept;
    for (std::size_t i = 0; i < pending->object->ElementCount(); ++i) {
      const Value entry = pending->object->GetElement(i);
      const Value* entry_signal = entry.IsObject() ? entry.object->GetOwn(kFetchSignalSlot) : nullptr;
      if (entry_signal == nullptr || !entry_signal->IsObject() ||
          entry_signal->object != signal.object) {
        kept.push_back(entry);
        continue;
      }
      if (network_ != nullptr) {
        const Value* id = entry.object->GetOwn(kFetchIdSlot);
        if (id != nullptr) {
          network_->AbortFetch(static_cast<std::uint64_t>(js::ToNumber(*id)));
        }
      }
      const Value* promise = entry.object->GetOwn(kFetchPromiseSlot);
      if (promise != nullptr && promise->IsObject()) {
        interpreter_->SettleAsyncResult(promise->object, why, true);
      }
      AddPerformanceCounter(PerfCounterId::FetchAborted);
    }
    pending->object->SetElements(kept, std::vector<bool>(kept.size(), true));
  }

  // The event last, after `aborted` is true and the fetches are gone, because
  // a handler reads both.
  const Value event = MakeEvent("abort", /*bubbles=*/false, /*cancelable=*/false,
                                /*trusted=*/true);
  if (event.IsObject()) {
    event.object->Set("target", signal);
    RunListenersOn(signal, event, "#on:abort", EventPhase::AtTarget);
  }
  interpreter_->DrainMicrotasks();
}

// --- fetch ------------------------------------------------------------------

void DomBindings::InstallFetch() {
  if (network_ == nullptr) {
    // No network behind this binding layer, so no `fetch` at all. An absence
    // rather than a stub: ADR 0012's rule, and the one place it matters most --
    // a page that finds `fetch` and gets a rejection has no fallback path left.
    return;
  }
  InstallHeaders();
  InstallResponse();
  InstallRequest();
  InstallAbortController();

  const Value fetch = interpreter_->NewNativeValue("fetch", [this](NativeCall& call) {
    const Value promise = call.interpreter.NewPromiseValue();
    if (!promise.IsObject()) {
      return Value::Undefined();
    }
    const auto reject = [&call, &promise](std::string message) {
      Value error = call.interpreter.MakeError("TypeError", std::move(message));
      call.interpreter.SettleAsyncResult(promise.object, error, true);
      return promise;
    };

    ScriptRequest request;
    const Value input = Argument(call.arguments, 0);
    if (input.IsObject() && input.object->GetOwn("url") != nullptr) {
      // A `Request`, or anything shaped like one. Its own `method` and headers
      // are the defaults an `init` then overrides, which is what makes
      // `fetch(new Request(url, a), b)` mean what a page expects.
      request.url = js::ToString(*input.object->Get("url"));
      if (const Value* method = input.object->Get("method")) {
        request.method = js::ToString(*method);
      }
      if (const Value* headers = input.object->Get("headers")) {
        for (const Value& pair : ReadPairs(*headers, kHeaderPairsSlot)) {
          request.headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
        }
      }
    } else {
      request.url = js::ToString(input);
    }
    if (request.url.empty()) {
      return reject("fetch requires a URL");
    }

    Value signal;
    const Value init = Argument(call.arguments, 1);
    if (init.IsObject()) {
      if (const Value* method = init.object->Get("method")) {
        request.method = js::ToString(*method);
      }
      if (const Value* mode = init.object->Get("mode")) {
        request.mode = js::ToString(*mode);
      }
      if (const Value* credentials = init.object->Get("credentials")) {
        request.credentials = js::ToString(*credentials);
      }
      if (const Value* body = init.object->Get("body")) {
        if (!body->IsUndefined() && !body->IsNull()) {
          request.body = js::ToString(*body);
        }
      }
      if (const Value* headers = init.object->Get("headers")) {
        request.headers.clear();
        if (headers->IsObject() && headers->object->GetOwn(kHeaderPairsSlot) != nullptr) {
          for (const Value& pair : ReadPairs(*headers, kHeaderPairsSlot)) {
            request.headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
          }
        } else if (headers->IsObject()) {
          for (const std::string& key : headers->object->EnumerableKeys()) {
            const Value* value = headers->object->Get(key);
            request.headers.push_back(ScriptHeader{
                LowerCase(key), value == nullptr ? std::string() : js::ToString(*value)});
          }
        }
      }
      if (const Value* given = init.object->Get("signal")) {
        signal = *given;
      }
    }

    // Dropped here as well as in `net`, because the specification drops them
    // here and a page that reads back what it set has to see the same thing
    // every other browser shows it.
    request.headers.erase(
        std::remove_if(request.headers.begin(), request.headers.end(),
                       [](const ScriptHeader& header) {
                         return IsForbiddenHeaderName(LowerCase(header.name));
                       }),
        request.headers.end());

    if (signal.IsObject()) {
      const Value* aborted = signal.object->GetOwn(kAbortedSlot);
      if (aborted != nullptr && js::ToBoolean(*aborted)) {
        // Already aborted before the request was made. Rejected without a
        // request, which is the difference between a signal and a flag.
        const Value* why = signal.object->GetOwn(kAbortReasonSlot);
        call.interpreter.SettleAsyncResult(promise.object,
                                           why == nullptr ? Value::Undefined() : *why, true);
        return promise;
      }
    }

    const std::uint64_t id = network_->StartFetch(request);
    if (id == 0) {
      return reject("failed to fetch");
    }
    AddPerformanceCounter(PerfCounterId::FetchRequests);

    const Value entry = call.interpreter.NewObjectValue();
    if (!entry.IsObject()) {
      return promise;
    }
    entry.object->SetHidden(kFetchIdSlot, Value::Number(static_cast<double>(id)));
    entry.object->SetHidden(kFetchPromiseSlot, promise);
    if (signal.IsObject()) {
      entry.object->SetHidden(kFetchSignalSlot, signal);
    }
    const Value pending = PendingFetches();
    if (pending.IsObject()) {
      pending.object->PushElement(entry);
    }
    return promise;
  });
  if (fetch.IsObject()) {
    fetch.object->Set(kOwnerSlot, PointerValue(this));
    interpreter_->Global()->Set("fetch", fetch);
    interpreter_->GlobalScope()->Declare("fetch", fetch, false);
  }
}

js::Value DomBindings::PendingFetches() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kPendingFetchSlot)) {
    return *existing;
  }
  const Value list = interpreter_->NewArrayValue({});
  if (list.IsObject()) {
    interfaces_.object->Set(kPendingFetchSlot, list);
  }
  return list;
}

bool DomBindings::DeliverFetchResponse(std::uint64_t id, const ScriptResponse& response) {
  if (interpreter_ == nullptr || !interfaces_.IsObject()) {
    return false;
  }
  // Deliberately not PendingFetches(), which would create the list: a document
  // that never called `fetch` must not allocate one to be told nothing arrived.
  const Value* pending = interfaces_.object->GetOwn(kPendingFetchSlot);
  if (pending == nullptr || !pending->IsObject()) {
    return false;
  }
  Value promise;
  std::vector<Value> kept;
  for (std::size_t i = 0; i < pending->object->ElementCount(); ++i) {
    const Value entry = pending->object->GetElement(i);
    const Value* entry_id = entry.IsObject() ? entry.object->GetOwn(kFetchIdSlot) : nullptr;
    if (entry_id == nullptr || static_cast<std::uint64_t>(js::ToNumber(*entry_id)) != id) {
      kept.push_back(entry);
      continue;
    }
    const Value* found = entry.object->GetOwn(kFetchPromiseSlot);
    if (found != nullptr) {
      promise = *found;
    }
  }
  if (!promise.IsObject()) {
    // An answer for a request nobody is waiting for: aborted, or delivered
    // twice. Dropping it is the right answer and the only safe one.
    return false;
  }
  pending->object->SetElements(kept, std::vector<bool>(kept.size(), true));

  if (!response.ok) {
    // One message for every network failure, and deliberately not the reason.
    // A cross-origin failure that explained itself -- "no
    // Access-Control-Allow-Origin" against "connection refused" -- would tell a
    // page whether the resource exists, which is the read CORS is there to
    // prevent. The reason goes to the console, where the developer is.
    Value error = interpreter_->MakeError("TypeError", "Failed to fetch");
    interpreter_->SettleAsyncResult(promise.object, error, true);
    AddPerformanceCounter(PerfCounterId::FetchFailed);
  } else {
    interpreter_->SettleAsyncResult(promise.object, MakeResponse(response), false);
    AddPerformanceCounter(PerfCounterId::FetchDelivered);
  }
  // The `then` handlers run here rather than at some later turn, because the
  // caller is the engine's turn boundary and this is what makes a fetch's
  // continuation part of the frame that delivered it.
  interpreter_->DrainMicrotasks();
  return true;
}

}  // namespace microbrowser::bindings
