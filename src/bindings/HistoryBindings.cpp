// `window.history`, and the two events a traversal fires.
//
// ADR 0026 §2. Three things here are the whole of the feature:
//
//   * **The origin check is on the other side of the interface.** `pushState`
//     hands the URL over as the page wrote it and gets back a decision. This
//     module may not see `url`, so it cannot compare origins -- which is the
//     point rather than a limitation: one origin comparison, in the module that
//     owns URLs. See bindings/History.h.
//   * **`history.state` is memoized.** Deserializing on every read would make
//     `history.state === history.state` false, and a page that stashes it and
//     compares later would see a change that did not happen. The cache is keyed
//     on a generation counter the engine bumps.
//   * **A traversal is requested, never performed.** `history.back()` returns
//     and the engine acts after the script turn ends, because a traversal can
//     replace the document and doing that with the interpreter on the stack is a
//     use-after-free.

#include <cstdint>
#include <string>
#include <utility>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/History.h"
#include "js/StructuredClone.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// The memoized `history.state` and the generation it was made for. On the
// interfaces object, which is already a GC root: a `js::Value` in a C++ field is
// invisible to the collector.
constexpr const char* kHistoryStateSlot = "#history:state";
constexpr const char* kHistoryGenerationSlot = "#history:generation";

}  // namespace

void DomBindings::InstallHistory() {
  if (history_ == nullptr) {
    // No history behind this binding layer, so no `history` object at all. The
    // absence ADR 0012 asks for: a page that finds `history.pushState` and gets
    // nothing has already taken the branch that assumes it works.
    return;
  }
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value history = interpreter_->NewObjectValue();
  if (!history.IsObject()) {
    return;
  }
  interfaces_.object->Set("History", history);

  const Value length = interpreter_->NewNativeValue("length", [this](NativeCall&) {
    return Value::Number(static_cast<double>(history_->HistoryLength()));
  });
  if (length.IsObject()) {
    length.object->Set(kOwnerSlot, PointerValue(this));
    history.object->DefineAccessor("length", length.object, nullptr);
  }

  const Value state = interpreter_->NewNativeValue("state", [this](NativeCall&) {
    return HistoryStateValue();
  });
  if (state.IsObject()) {
    state.object->Set(kOwnerSlot, PointerValue(this));
    history.object->DefineAccessor("state", state.object, nullptr);
  }

  // `scrollRestoration` is deliberately absent rather than a settable string
  // that changes nothing: a page that sets it to "manual" and then restores the
  // scroll itself would scroll twice, which is worse than a page that finds the
  // name missing and does not try.

  const auto install_push = [this, &history](const char* name, bool replace) {
    const Value method =
        interpreter_->NewNativeValue(name, [this, replace, name](NativeCall& call) {
          const Value given = Argument(call.arguments, 0);
          const std::optional<js::SerializedValue> serialized =
              js::StructuredSerialize(call.interpreter, given);
          if (!serialized.has_value()) {
            // What the specification calls a DataCloneError, and what a page
            // that put a callback in its state actually did.
            Value error = call.interpreter.MakeError(
                "Error", std::string(name) + ": the state object could not be cloned");
            if (error.IsObject()) {
              error.object->Set("name", Value::String("DataCloneError"));
            }
            return call.ThrowValue(error);
          }
          // Argument 1 is the title, which the specification requires and every
          // browser ignores. Read and dropped, so that a page passing three
          // arguments is not a page passing the URL as the title.
          const Value url_argument = Argument(call.arguments, 2);
          std::string url;
          if (!url_argument.IsUndefined() && !url_argument.IsNull()) {
            if (!CoerceToString(call, url_argument, url)) {
              return call.ThrownValue();
            }
          }

          switch (history_->PushHistoryState(*serialized, url, replace)) {
            case HistorySource::UrlOutcome::Ok:
              break;
            case HistorySource::UrlOutcome::Unparseable: {
              Value error = call.interpreter.MakeError(
                  "Error", std::string(name) + ": '" + url + "' is not a valid URL");
              if (error.IsObject()) {
                error.object->Set("name", Value::String("SyntaxError"));
              }
              return call.ThrowValue(error);
            }
            case HistorySource::UrlOutcome::NotSameOrigin: {
              // The refusal the whole feature is careful about. A page that
              // could move the URL bar to another origin could spoof any site
              // it liked, so this is a throw rather than a silent no-op --
              // ADR 0026 §2.
              AddPerformanceCounter(PerfCounterId::HistoryOriginRefusals);
              return ThrowDom(call, "SecurityError",
                              std::string(name) + ": '" + url +
                                  "' is not the document's origin");
            }
          }
          AddPerformanceCounter(replace ? PerfCounterId::HistoryReplaceStates
                                        : PerfCounterId::HistoryPushStates);
          // The memo is stale now, and the generation the engine bumped is what
          // says so -- but dropping it here as well means the next read cannot
          // possibly answer with the previous state.
          InvalidateHistoryState();
          return Value::Undefined();
        });
    if (method.IsObject()) {
      method.object->Set(kOwnerSlot, PointerValue(this));
      history.object->Set(name, method);
    }
  };
  install_push("pushState", false);
  install_push("replaceState", true);

  const auto install_traversal = [this, &history](const char* name, int fixed_delta) {
    const Value method =
        interpreter_->NewNativeValue(name, [this, fixed_delta](NativeCall& call) {
          int delta = fixed_delta;
          if (fixed_delta == 0) {
            // `history.go()` and `history.go(0)` reload in the specification.
            // Not here: a reload from script is a navigation the page did not
            // name a destination for, and treating "no argument" as one is how
            // `go(undefined)` becomes a reload loop. Zero traverses nothing.
            const Value argument = Argument(call.arguments, 0);
            delta = argument.IsUndefined() ? 0 : static_cast<int>(js::ToNumber(argument));
          }
          if (delta != 0) {
            history_->RequestHistoryTraversal(delta);
          }
          return Value::Undefined();
        });
    if (method.IsObject()) {
      method.object->Set(kOwnerSlot, PointerValue(this));
      history.object->Set(name, method);
    }
  };
  install_traversal("back", -1);
  install_traversal("forward", 1);
  install_traversal("go", 0);

  interpreter_->Global()->Set("history", history);
  interpreter_->GlobalScope()->Declare("history", history, false);
}

js::Value DomBindings::HistoryStateValue() {
  if (history_ == nullptr || interpreter_ == nullptr || !interfaces_.IsObject()) {
    return Value::Null();
  }
  const std::uint64_t generation = history_->HistoryStateGeneration();
  const Value* cached_generation = interfaces_.object->GetOwn(kHistoryGenerationSlot);
  if (cached_generation != nullptr &&
      static_cast<std::uint64_t>(js::ToNumber(*cached_generation)) == generation) {
    const Value* cached = interfaces_.object->GetOwn(kHistoryStateSlot);
    if (cached != nullptr) {
      return *cached;
    }
  }
  const js::SerializedValue& bytes = history_->HistoryState();
  // Null and not undefined: `history.state` is null on an entry nobody gave a
  // state to, and a page tests `!== null`.
  const Value value =
      bytes.Empty() ? Value::Null() : js::StructuredDeserialize(*interpreter_, bytes);
  interfaces_.object->SetHidden(kHistoryStateSlot, value);
  interfaces_.object->SetHidden(kHistoryGenerationSlot,
                                Value::Number(static_cast<double>(generation)));
  return value;
}

void DomBindings::InvalidateHistoryState() {
  if (interfaces_.IsObject()) {
    interfaces_.object->SetHidden(kHistoryGenerationSlot, Value::Number(-1.0));
  }
}

bool DomBindings::DispatchPopState() {
  if (interpreter_ == nullptr) {
    return false;
  }
  InvalidateHistoryState();
  const Value event = MakeEvent("popstate", /*bubbles=*/false, /*cancelable=*/false,
                                /*trusted=*/true);
  if (!event.IsObject()) {
    return false;
  }
  // The state is on the event as well as on `history`, because a handler reads
  // `event.state` -- and it has to be the *same* object, or a page that compares
  // the two sees a change that did not happen.
  event.object->Set("state", HistoryStateValue());
  const bool ran = DispatchAtWindowWith("popstate", event);
  interpreter_->DrainMicrotasks();
  return ran;
}

bool DomBindings::DispatchHashChange(const std::string& old_url, const std::string& new_url) {
  if (interpreter_ == nullptr) {
    return false;
  }
  const Value event = MakeEvent("hashchange", /*bubbles=*/false, /*cancelable=*/false,
                                /*trusted=*/true);
  if (!event.IsObject()) {
    return false;
  }
  // Its own interface, not Event's. `MakeEvent` gives every event
  // `Event.prototype`, which is right for the ones with no interface of their
  // own -- but a router that does `if (e instanceof HashChangeEvent)` before
  // reading `e.newURL` is checking the one thing that distinguishes this event
  // from every other, and it answered false.
  if (const Value prototype = InterfaceNamed("HashChangeEvent"); prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  event.object->Set("oldURL", Value::String(old_url));
  event.object->Set("newURL", Value::String(new_url));
  const bool ran = DispatchAtWindowWith("hashchange", event);
  interpreter_->DrainMicrotasks();
  return ran;
}

}  // namespace microbrowser::bindings
