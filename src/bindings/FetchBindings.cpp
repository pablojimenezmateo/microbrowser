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
#include <string_view>
#include <utility>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/FetchSupport.h"
#include "bindings/TrustedScript.h"
#include "bindings/Network.h"
#include "bindings/Timers.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// A composite signal's two lists, and the flag that says it is one.
//
// `AbortSignal.any` is not "add a listener to each source": the DOM keeps the
// relationship both ways and *flattens* it, so a signal composed from a
// composed signal is linked to the original sources rather than to the
// intermediate. That is what makes the abort events fire in creation order --
// source first, then every dependent in the order it was made -- and what lets
// every dependent be marked aborted before any of their handlers run.
constexpr const char* kSignalDependentsSlot = "#dependents";
constexpr const char* kSignalSourcesSlot = "#sources";
constexpr const char* kSignalIsDependentSlot = "#isDependent";

bool SignalAborted(const Value& signal) {
  if (!signal.IsObject()) {
    return false;
  }
  const Value* flag = signal.object->GetOwn(kAbortedSlot);
  return flag != nullptr && js::ToBoolean(*flag);
}

Value SignalReason(const Value& signal) {
  if (!signal.IsObject()) {
    return Value::Undefined();
  }
  const Value* reason = signal.object->GetOwn(kAbortReasonSlot);
  return reason == nullptr ? Value::Undefined() : *reason;
}

// "Set signal's abort reason": aborted and why, and nothing else. Separate
// from firing the event because the DOM marks every dependent signal *before*
// any of their abort handlers run -- a handler that composes a new signal from
// one of them must see it already aborted.
void MarkAborted(const Value& signal, const Value& reason) {
  if (!signal.IsObject()) {
    return;
  }
  signal.object->SetHidden(kAbortedSlot, Value::Bool(true));
  signal.object->SetHidden(kAbortReasonSlot, reason);
}

// The array in `slot`, created empty if it is not there yet.
Value SignalList(js::Interpreter& interpreter, const Value& signal, const char* slot) {
  if (!signal.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = signal.object->GetOwn(slot);
      existing != nullptr && existing->IsObject()) {
    return *existing;
  }
  const Value made = interpreter.NewArrayValue({});
  if (made.IsObject()) {
    signal.object->SetHidden(slot, made);
  }
  return made;
}

bool ListContains(const Value& list, const Value& value) {
  if (!list.IsObject() || !value.IsObject()) {
    return false;
  }
  for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
    const Value each = list.object->GetElement(i);
    if (each.IsObject() && each.object == value.object) {
      return true;
    }
  }
  return false;
}

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

  // `throwIfAborted()`, which is the reason a page holds a signal it did not
  // make: it turns "did someone cancel me" into one line at the top of an
  // async step, and the thing it throws is the reason itself rather than a
  // wrapper -- a page that aborted with a string gets its string back.
  const Value throw_if_aborted =
      interpreter_->NewNativeValue("throwIfAborted", [](NativeCall& call) -> Value {
        if (!SignalAborted(call.self)) {
          return Value::Undefined();
        }
        return call.ThrowValue(SignalReason(call.self));
      });
  if (throw_if_aborted.IsObject()) {
    signal_prototype.object->Set("throwIfAborted", throw_if_aborted);
  }

  // One signal, made the one way. Every signal in this browser comes from
  // here -- a controller's, `abort()`'s, `timeout()`'s and `any()`'s -- because
  // four places building the same object is four chances for one of them to
  // forget it is an event target.
  DomBindings* self = this;
  const auto make_signal = [self, signal_prototype](js::Interpreter& interpreter) {
    const Value signal = interpreter.NewObjectValue();
    if (!signal.IsObject()) {
      return signal;
    }
    signal.object->SetPrototype(signal_prototype.object);
    signal.object->SetHidden(kAbortedSlot, Value::Bool(false));
    // An event target, so `signal.addEventListener('abort', …)` works --
    // which is how every library that composes signals listens for one.
    self->InstallEventMethods(signal);
    return signal;
  };

  // `AbortSignal` itself, as a name. A page cannot construct one -- that is a
  // TypeError in a browser too, because the only way to get a signal is off a
  // controller or one of the statics below -- but the *name* has to resolve,
  // for two reasons a measured page gives: `signal instanceof AbortSignal`, and
  // the shape youtube's bundle uses, which is to feature-detect
  // `AbortController` and then reach for `AbortSignal` unqualified:
  //
  //     var $Y2 = typeof AbortController === "function";
  //     var xh = $Y2 ? AbortSignal : <its own polyfill>;
  //     typeof xh.timeout !== "function" && (xh.timeout = ...)
  //
  // So a browser with `AbortController` and no `AbortSignal` is a shape the web
  // does not have: the detection passes and the next line throws a
  // ReferenceError, which is worse than either having both or having neither.
  const Value signal_constructor =
      interpreter_->NewNativeValue("AbortSignal", [](NativeCall& call) {
        return call.Throw("TypeError", "Illegal constructor: AbortSignal");
      });
  if (signal_constructor.IsObject()) {
    signal_constructor.object->Set("prototype", signal_prototype);
    signal_prototype.object->Set("constructor", signal_constructor);
    // `AbortSignal.abort(reason)`: a signal that is already aborted. It fires
    // nothing, because there was never a moment at which it was not -- a page
    // that adds an `abort` listener to one is asking about an event that has
    // already gone past.
    const Value abort_static =
        interpreter_->NewNativeValue("abort", [make_signal](NativeCall& call) {
          const Value signal = make_signal(call.interpreter);
          Value why = Argument(call.arguments, 0);
          if (why.IsUndefined()) {
            why = MakeDomException(call.interpreter, "AbortError",
                                   "signal is aborted without reason");
          }
          MarkAborted(signal, why);
          return signal;
        });
    if (abort_static.IsObject()) {
      signal_constructor.object->Set("abort", abort_static);
    }
    // `AbortSignal.timeout(ms)`: the browser's own deadline, not the page's.
    // The reason is a `TimeoutError` rather than an `AbortError`, which is the
    // whole point of having it -- a caller distinguishes "the user cancelled"
    // from "it took too long" by the name on the exception.
    const Value timeout_static = interpreter_->NewNativeValue(
        "timeout", [self, make_signal](NativeCall& call) -> Value {
          // `unsigned long long` in the IDL; `unsigned long` is the width this
          // module converts and the difference is 49 days of delay, which is
          // past the queue's own clamp either way.
          std::uint32_t delay = 0;
          if (!ToUnsignedLong(call, Argument(call.arguments, 0), IntegerRange::Modulo, delay)) {
            return call.ThrownValue();
          }
          const Value signal = make_signal(call.interpreter);
          const Value fire = call.interpreter.NewNativeValue("#timeout", [self](NativeCall& inner) {
            if (inner.callee != nullptr) {
              if (const Value* which = inner.callee->GetOwn("#signal")) {
                self->AbortSignalled(*which,
                                     MakeDomException(inner.interpreter, "TimeoutError",
                                                      "signal timed out"));
              }
            }
            return Value::Undefined();
          });
          if (fire.IsObject()) {
            // On the function object rather than captured: a capture is
            // invisible to the collector, and this one would outlive the
            // signal it points at by however long the delay is.
            fire.object->SetHidden("#signal", signal);
            TimerQueue::QueueDelayedTask(call.interpreter, fire,
                                         static_cast<std::int64_t>(delay));
          }
          return signal;
        });
    if (timeout_static.IsObject()) {
      signal_constructor.object->Set("timeout", timeout_static);
    }
    // `AbortSignal.any(signals)`, DOM §3.3. The flattening in step 4 is the
    // part that is easy to get wrong and is observable: a signal composed from
    // a composed signal links to the *original* sources, so the abort events
    // fire source-first and then in the order the dependents were created,
    // however deep the composition goes.
    const Value any_static =
        interpreter_->NewNativeValue("any", [make_signal](NativeCall& call) -> Value {
          if (!RequireArguments(call, "AbortSignal", "any", 1)) {
            return call.ThrownValue();
          }
          // `sequence<AbortSignal>`. Read as an array rather than through the
          // iteration protocol, which is what this module converts a sequence
          // with everywhere else: a page passes an array here, and a generator
          // would want an iterator helper that has no other caller.
          std::vector<Value> given;
          if (call.arguments[0].IsObject()) {
            js::Object* list = call.arguments[0].object;
            for (std::size_t i = 0; i < list->ElementCount(); ++i) {
              given.push_back(list->GetElement(i));
            }
          }
          const Value result = make_signal(call.interpreter);
          // An already-aborted source wins immediately, with *its* reason
          // object rather than a copy -- pages compare the two by identity.
          for (const Value& source : given) {
            if (SignalAborted(source)) {
              MarkAborted(result, SignalReason(source));
              return result;
            }
          }
          result.object->SetHidden(kSignalIsDependentSlot, Value::Bool(true));
          const Value sources = SignalList(call.interpreter, result, kSignalSourcesSlot);
          const auto link = [&call, &result, &sources](const Value& source) {
            if (!source.IsObject() || ListContains(sources, source)) {
              return;
            }
            sources.object->PushElement(source);
            const Value dependents =
                SignalList(call.interpreter, source, kSignalDependentsSlot);
            if (dependents.IsObject()) {
              dependents.object->PushElement(result);
            }
          };
          for (const Value& source : given) {
            if (!source.IsObject()) {
              continue;
            }
            const Value* dependent = source.object->GetOwn(kSignalIsDependentSlot);
            if (dependent == nullptr || !js::ToBoolean(*dependent)) {
              link(source);
              continue;
            }
            const Value* inner = source.object->GetOwn(kSignalSourcesSlot);
            if (inner == nullptr || !inner->IsObject()) {
              continue;
            }
            for (std::size_t i = 0; i < inner->object->ElementCount(); ++i) {
              link(inner->object->GetElement(i));
            }
          }
          return result;
        });
    if (any_static.IsObject()) {
      signal_constructor.object->Set("any", any_static);
    }
    interpreter_->Global()->Set("AbortSignal", signal_constructor);
    interpreter_->GlobalScope()->Declare("AbortSignal", signal_constructor, false);
  }

  const Value constructor =
      interpreter_->NewNativeValue("AbortController", [this, make_signal](NativeCall& call) {
        const Value controller = call.interpreter.NewObjectValue();
        const Value signal = make_signal(call.interpreter);
        if (!controller.IsObject() || !signal.IsObject()) {
          return Value::Undefined();
        }
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
          abort.object->Set(kOwnerSlot, OwnerValue(this));
          controller.object->Set("abort", abort);
        }
        return controller;
      });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, OwnerValue(this));
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
    why = MakeDomException(*interpreter_, "AbortError", "The operation was aborted");
  }
  MarkAborted(signal, why);

  // **Every dependent signal is marked before any handler runs**, DOM §3.2
  // step 4. Not "and then aborted in turn": a handler on the source that
  // composes a *new* signal from a dependent has to find that dependent
  // already aborted, and one that reads `signal.aborted` on any of them has to
  // see true. Firing as we went would leave the last few still false for the
  // duration of the first one's handler.
  std::vector<Value> dependents;
  if (const Value* linked = signal.object->GetOwn(kSignalDependentsSlot);
      linked != nullptr && linked->IsObject()) {
    for (std::size_t i = 0; i < linked->object->ElementCount(); ++i) {
      const Value each = linked->object->GetElement(i);
      if (each.IsObject() && !SignalAborted(each)) {
        MarkAborted(each, why);
        dependents.push_back(each);
      }
    }
  }

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

  // The events last, after `aborted` is true on the whole family and the
  // fetches are gone, because a handler reads all of it. Source first, then
  // the dependents in the order they were composed -- which is why `any`
  // flattens rather than chaining: a chain would fire them depth-first.
  const auto fire = [this](const Value& target) {
    const Value event = MakeEvent("abort", /*bubbles=*/false, /*cancelable=*/false,
                                  /*trusted=*/true);
    if (event.IsObject()) {
      event.object->Set("target", target);
      RunListenersOn(target, event, "#on:abort", EventPhase::AtTarget);
    }
  };
  fire(signal);
  for (const Value& dependent : dependents) {
    fire(dependent);
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
  InstallReadableStream();
  InstallResponse();
  InstallRequest();
  // After both, because it hangs `formData()` off each of their prototypes.
  InstallFormData();
  InstallAbortController();
  // Over the same machinery, and installed here rather than beside it in
  // `Install` so that "there is a network behind this layer" is asked once.
  InstallXhr();

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
    Value signal;
    if (input.IsObject() && input.object->GetOwn("url") != nullptr) {
      // A `Request`, or anything shaped like one. Its own `method`, headers,
      // body and credentials are the defaults an `init` then overrides, which
      // is what makes `fetch(new Request(url, a), b)` mean what a page expects.
      // YouTube SABR is exactly that shape: `fetch(new Request(url, {method,
      // body, credentials}))` with no second argument.
      if (!CoerceToString(call, *input.object->Get("url"), request.url)) {
        return call.ThrownValue();
      }
      if (const Value* method = input.object->Get("method")) {
        if (!CoerceToString(call, *method, request.method)) {
          return call.ThrownValue();
        }
      }
      if (const Value* mode = input.object->Get("mode")) {
        if (!CoerceToString(call, *mode, request.mode)) {
          return call.ThrownValue();
        }
      }
      if (const Value* credentials = input.object->Get("credentials")) {
        if (!CoerceToString(call, *credentials, request.credentials)) {
          return call.ThrownValue();
        }
      }
      if (const Value* headers = input.object->Get("headers")) {
        for (const Value& pair : ReadPairs(*headers, kHeaderPairsSlot)) {
          request.headers.push_back(ScriptHeader{PairPart(pair, 0), PairPart(pair, 1)});
        }
      }
      if (const Value* body = input.object->GetOwn(kRequestBodySlot)) {
        if (body->IsString()) {
          request.body = body->AsString();
        } else if (!CoerceToString(call, *body, request.body)) {
          return call.ThrownValue();
        }
        const Value* from_string = input.object->GetOwn(kRequestBodyFromStringSlot);
        request.body_from_string =
            from_string != nullptr && js::ToBoolean(*from_string);
      }
      if (const Value* given = input.object->GetOwn(kRequestSignalSlot)) {
        signal = *given;
      }
    } else if (!CoerceToString(call, input, request.url)) {
      // `fetch(location)` / `fetch(new URL(...))` must see href via toString,
      // not the pure `js::ToString` "[object Object]" path.
      return call.ThrownValue();
    }
    // A plain object with no Request shape becomes "[object Object]" via
    // OrdinaryToPrimitive. Chrome's `fetch({})` throws TypeError *before* a
    // request; resolving that string against the document base produced
    // `https://www.youtube.com/[object%20Object]` (and a consent continue=
    // carrying the same path). Refuse the Object.prototype.toString form.
    if (request.url == "[object Object]") {
      return reject("Failed to parse URL from [object Object]");
    }
    if (request.url.empty()) {
      return reject("fetch requires a URL");
    }

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
          bool from_string = false;
          if (!ExtractRequestBody(*body, request.body, from_string)) {
            return reject("failed to read request body");
          }
          request.body_from_string = from_string;
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
    entry.object->SetHidden(kFetchTrustSlot, Value::Bool(InTrustedScriptContext()));
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
    fetch.object->Set(kOwnerSlot, OwnerValue(this));
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
  Value xhr;
  bool trust_scripts = false;
  std::vector<Value> kept;
  for (std::size_t i = 0; i < pending->object->ElementCount(); ++i) {
    const Value entry = pending->object->GetElement(i);
    const Value* entry_id = entry.IsObject() ? entry.object->GetOwn(kFetchIdSlot) : nullptr;
    if (entry_id == nullptr || static_cast<std::uint64_t>(js::ToNumber(*entry_id)) != id) {
      kept.push_back(entry);
      continue;
    }
    if (const Value* trust = entry.object->GetOwn(kFetchTrustSlot)) {
      trust_scripts = js::ToBoolean(*trust);
    }
    // A promise for a `fetch`, an XHR object for an `XMLHttpRequest`. One table
    // and one delivery, which is ADR 0020 §1's rule that there is one request
    // path -- the two shapes differ only in what is settled at the end of it.
    if (const Value* found = entry.object->GetOwn(kFetchPromiseSlot)) {
      promise = *found;
    }
    if (const Value* found = entry.object->GetOwn(kXhrSlot)) {
      xhr = *found;
    }
  }
  if (!promise.IsObject() && !xhr.IsObject()) {
    // An answer for a request nobody is waiting for: aborted, or delivered
    // twice. Dropping it is the right answer and the only safe one.
    return false;
  }
  pending->object->SetElements(kept, std::vector<bool>(kept.size(), true));

  // A network answer is a new host task. NetworkTaskBudget zeros under live
  // frames and raises the hang-guard for SABR UMP→appendBuffer (TD-0042 /
  // TD-0049); BeginHostTurn alone left soft-nav with buffers and zero appends.
  js::Interpreter::NetworkTaskBudget network_budget(*interpreter_);
  TrustedScriptInvocation trust(*interpreter_, trust_scripts);

  if (xhr.IsObject()) {
    DeliverToXhr(xhr, response);
    return true;
  }

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
