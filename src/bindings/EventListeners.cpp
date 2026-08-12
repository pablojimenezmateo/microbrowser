#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Registering and unregistering a listener -- `addEventListener` and
// `removeEventListener`, and nothing else.
//
// Split out of EventBindings.cpp when that file reached the module's line cap,
// along the seam it already had: the rest of that file is about *events* --
// what one is, how it is constructed, what dispatching one does -- and this is
// about the list a target keeps. The two halves share only the shape of a
// listener entry, which is why the slot names travel with this file: the entry
// is this file's format and the dispatch loop is its only other reader.
//
// Neither function is a member of DomBindings and neither needs to be. A
// listener list lives on the receiver, keyed by type, and nothing here asks a
// question about the document -- which is also why `new EventTarget()` works
// without a line of special-casing.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where a listener with flags keeps the function it wraps. A listener is
// stored as the function itself unless it needs a flag, which keeps the common
// case one object rather than two -- and the common case is most of the 930
// registrations ADR 0017 counted.
constexpr const char* kListenerFunctionSlot = "#fn";
constexpr const char* kListenerCaptureSlot = "#capture";
constexpr const char* kListenerPassiveSlot = "#passive";
constexpr const char* kListenerOnceSlot = "#once";

// What an `AbortSignal` passed to `addEventListener` remembers, so aborting it
// can find the one entry it was given to. Held as properties on the native
// removal function rather than as captures in its std::function, because a
// capture is invisible to the collector and this module has had that bug once
// already: the target and the entry would be freed while the signal still
// pointed at them.
constexpr const char* kAbortTargetSlot = "#abortTarget";
constexpr const char* kAbortEntrySlot = "#abortEntry";
constexpr const char* kAbortListSlot = "#abortList";

Value ListenerFunction(const Value& entry) {
  if (!entry.IsObject()) {
    return Value::Undefined();
  }
  if (entry.object->IsCallable()) {
    return entry;
  }
  const Value* wrapped = entry.object->GetOwn(kListenerFunctionSlot);
  return wrapped == nullptr ? Value::Undefined() : *wrapped;
}

// A flag on a listener entry. A bare function has none of them, which is why
// the fast path never allocates.
bool ListenerFlag(const Value& entry, const char* slot) {
  if (!entry.IsObject() || entry.object->IsCallable()) {
    return false;
  }
  const Value* found = entry.object->GetOwn(slot);
  return found != nullptr && js::ToBoolean(*found);
}

// Whether an options argument asked for something. `addEventListener(t, f,
// true)` is the capture boolean rather than an options object, and reading a
// property off a boolean must not be an error.
bool Option(const Value& options, const char* name) {
  if (!options.IsObject()) {
    return false;
  }
  const Value* found = options.object->Get(name);
  return found != nullptr && js::ToBoolean(*found);
}

// Whether a dictionary member was *given*, as against given as false. WebIDL
// treats an `undefined` member as absent, and `passive` is the one option here
// where the difference is observable: omitted means "let the browser decide"
// and `false` means "I will cancel this, do not start scrolling without me".
bool OptionGiven(const Value& options, const char* name) {
  if (!options.IsObject()) {
    return false;
  }
  const Value* found = options.object->Get(name);
  return found != nullptr && !found->IsUndefined();
}

// `addEventListener(type, fn, true)` and `addEventListener(type, fn, {capture:
// true})` mean the same thing, and the third argument is a boolean far more
// often than it is an object.
bool CaptureOption(const Value& options) {
  if (options.IsObject()) {
    return Option(options, "capture");
  }
  return js::ToBoolean(options);
}

// The body element of `document`: the first `body` or `frameset` child of the
// document element. HTML's own definition, and the reason it is not "the first
// body anywhere" is that a `<body>` nested in a template or a foreign element
// is not the document's body.
const dom::Element* BodyElementOf(const dom::Document& document) {
  const dom::Element* root = document.DocumentElement();
  if (root == nullptr) {
    return nullptr;
  }
  for (const std::unique_ptr<dom::Node>& child : root->Children()) {
    if (!child->IsElement()) {
      continue;
    }
    const auto& element = static_cast<const dom::Element&>(*child);
    if (element.Namespace().IsHtml() &&
        (element.LocalName() == "body" || element.LocalName() == "frameset")) {
      return &element;
    }
  }
  return nullptr;
}

// DOM §2.7, "default passive value": what `passive` means when a page did not
// say. For the four scroll-blocking event types, on the four targets a page
// registers a global scroll handler on, it is **true**.
//
// This is not a nicety and it is not an optimisation this browser has yet
// earned. It is observable: a listener that is passive by default has its
// `preventDefault()` ignored, so `event.defaultPrevented` stays false and
// `dispatchEvent` answers true. Thirty-one subtests in passive-by-default.html
// assert exactly that, and a page that relies on the platform *not* letting it
// cancel a wheel event on `window` behaves differently here without it.
//
// The rule is per (type, target) rather than per listener, which is why it
// lives here rather than in the dispatch loop: by the time the event runs, the
// listener has to already know what it promised.
bool DefaultPassiveValue(std::string_view type, const Value& target, const js::Object* global) {
  if (type != "touchstart" && type != "touchmove" && type != "wheel" &&
      type != "mousewheel") {
    return false;
  }
  if (target.IsObject() && target.object == global) {
    return true;  // the Window
  }
  const dom::Node* node = NodeOf(target);
  if (node == nullptr) {
    return false;
  }
  const dom::Document* document = node->NodeDocument();
  if (document == nullptr) {
    return false;
  }
  return node == document || node == document->DocumentElement() ||
         node == BodyElementOf(*document);
}

// Whether `value` is an AbortSignal -- by walking its prototype chain to the
// one `AbortSignal.prototype` this realm has, rather than by looking for a
// property, because a page's own `{aborted: false}` is not a signal and
// AddEventListenerOptions' `signal` member is not nullable: anything else is a
// TypeError before a listener is added.
bool IsAbortSignal(js::Interpreter& interpreter, const Value& value) {
  if (!value.IsObject()) {
    return false;
  }
  const Value* constructor = interpreter.Global()->GetOwn("AbortSignal");
  if (constructor == nullptr || !constructor->IsObject()) {
    return false;
  }
  const Value* prototype = constructor->object->GetOwn("prototype");
  if (prototype == nullptr || !prototype->IsObject()) {
    return false;
  }
  for (js::Object* walk = value.object->Prototype(); walk != nullptr; walk = walk->Prototype()) {
    if (walk == prototype->object) {
      return true;
    }
  }
  return false;
}

// Removes one entry from a listener list by identity. Shared with the abort
// path below and with `removeEventListener`, which is why it is a function
// rather than a loop written twice.
void RemoveEntry(const Value& listeners, const Value& entry) {
  if (!listeners.IsObject()) {
    return;
  }
  std::vector<Value> kept;
  for (std::size_t i = 0; i < listeners.object->ElementCount(); ++i) {
    const Value each = listeners.object->GetElement(i);
    if (!js::StrictEquals(each, entry)) {
      kept.push_back(each);
    }
  }
  listeners.object->SetElements(std::move(kept), {});
}

}  // namespace

bool ListenerStillRegistered(const js::Value& listeners, const js::Value& entry) {
  if (!listeners.IsObject()) {
    return false;
  }
  for (std::size_t i = 0; i < listeners.object->ElementCount(); ++i) {
    if (js::StrictEquals(listeners.object->GetElement(i), entry)) {
      return true;
    }
  }
  return false;
}

void InstallListenerRegistration(js::Interpreter& interpreter, const js::Value& wrapper,
                                 const void* owner) {
  const auto method = [&interpreter, &wrapper, owner](const char* name,
                                                      js::NativeFunction function) {
    const Value native = interpreter.NewNativeValue(name, std::move(function));
    if (native.IsObject() && wrapper.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(owner));
      wrapper.object->Set(name, native);
    }
  };
  // Listeners live on the wrapper, keyed by type, so they are collected with
  // it and cannot outlive the node they were registered on.
  method("addEventListener", [](NativeCall& call) -> Value {
    // A bare `addEventListener('load', f)` is a call with no receiver, and in
    // a browser that registers on the window because sloppy-mode `this` is the
    // global object. Here it arrives as undefined, so the window is named
    // explicitly -- otherwise half the pages that listen for `load` listen on
    // nothing and are never told.
    const Value target =
        call.self.IsObject() ? call.self : Value::Obj(call.interpreter.Global());
    if (!target.IsObject()) {
      return call.Throw("TypeError", "addEventListener called on a non-node");
    }
    const Value handler = Argument(call.arguments, 1);
    const Value options = Argument(call.arguments, 2);
    // **The options dictionary is converted before the callback is looked at**,
    // which is WebIDL's own order and is observable: `addEventListener('foo',
    // null, {signal: null})` throws a TypeError for the signal rather than
    // quietly doing nothing for the null listener.
    Value signal;
    if (options.IsObject()) {
      if (const Value* given = options.object->Get("signal");
          given != nullptr && !given->IsUndefined()) {
        if (!IsAbortSignal(call.interpreter, *given)) {
          return call.Throw("TypeError",
                            "the signal option is not an AbortSignal");
        }
        signal = *given;
      }
    }
    if (!handler.IsObject() || !handler.object->IsCallable()) {
      // A non-callable listener is ignored rather than refused, which is what
      // the specification says and what stops a page's optional callback from
      // being fatal.
      return Value::Undefined();
    }
    // An already-aborted signal means the listener is never added at all --
    // not added and then removed, which would be observable through a `once`
    // listener the abort would have to un-fire.
    if (signal.IsObject()) {
      const Value* aborted = signal.object->Get("aborted");
      if (aborted != nullptr && js::ToBoolean(*aborted)) {
        return Value::Undefined();
      }
    }
    const std::string type = js::ToString(Argument(call.arguments, 0));
    const std::string slot = "#on:" + type;
    // The flags are a wrapper around the function rather than fields beside it,
    // so the list stays one array and removal stays identity on the function.
    // A listener with no flags -- which is most of them -- is stored as itself.
    const bool capture = CaptureOption(options);
    // Omitted is not the same as false here -- see DefaultPassiveValue.
    const bool passive = OptionGiven(options, "passive")
                             ? Option(options, "passive")
                             : DefaultPassiveValue(type, target, call.interpreter.Global());
    const bool once = Option(options, "once");
    const Value* existing = target.object->GetOwn(slot);
    // **A duplicate is not added.** The DOM's identity for a listener is
    // (type, callback, capture), and re-registering one that is already there
    // is a no-op -- so a page that arms the same handler on every render ends
    // up with one, and `removeEventListener` once really removes it. Without
    // this it took as many removals as registrations, which is a leak that
    // looks like a handler firing twice.
    if (existing != nullptr && existing->IsObject()) {
      for (std::size_t i = 0; i < existing->object->ElementCount(); ++i) {
        const Value each = existing->object->GetElement(i);
        if (js::StrictEquals(ListenerFunction(each), handler) &&
            ListenerFlag(each, kListenerCaptureSlot) == capture) {
          return Value::Undefined();
        }
      }
    }
    Value entry = handler;
    if (capture || passive || once) {
      const Value flagged = call.interpreter.NewObjectValue();
      if (flagged.IsObject()) {
        flagged.object->Set(kListenerFunctionSlot, handler);
        if (capture) flagged.object->Set(kListenerCaptureSlot, Value::Bool(true));
        if (passive) flagged.object->Set(kListenerPassiveSlot, Value::Bool(true));
        if (once) flagged.object->Set(kListenerOnceSlot, Value::Bool(true));
        entry = flagged;
      }
    }
    Value listeners;
    if (existing != nullptr && existing->IsObject()) {
      existing->object->PushElement(entry);
      listeners = *existing;
    } else {
      listeners = call.interpreter.NewArrayValue({entry});
      target.object->Set(slot, listeners);
    }
    // The signal's removal step, registered as an ordinary `abort` listener on
    // the signal. The DOM calls this an "abort algorithm" and runs it before
    // the abort event's own listeners; here it *is* one of them, registered
    // first, which gets the same order for the same reason -- a page's abort
    // handler sees a target the removal has already happened to.
    if (signal.IsObject()) {
      const Value forget =
          call.interpreter.NewNativeValue("#forgetListener", [](NativeCall& inner) {
            if (inner.callee == nullptr) {
              return Value::Undefined();
            }
            const Value* list = inner.callee->GetOwn(kAbortListSlot);
            const Value* dead = inner.callee->GetOwn(kAbortEntrySlot);
            if (list != nullptr && dead != nullptr) {
              RemoveEntry(*list, *dead);
            }
            return Value::Undefined();
          });
      if (forget.IsObject()) {
        forget.object->SetHidden(kAbortTargetSlot, target);
        forget.object->SetHidden(kAbortEntrySlot, entry);
        forget.object->SetHidden(kAbortListSlot, listeners);
        const Value* abort_listeners = signal.object->GetOwn("#on:abort");
        if (abort_listeners != nullptr && abort_listeners->IsObject()) {
          abort_listeners->object->PushElement(forget);
        } else {
          signal.object->Set("#on:abort", call.interpreter.NewArrayValue({forget}));
        }
      }
    }
    return Value::Undefined();
  });
  method("removeEventListener", [](NativeCall& call) {
    const Value target =
        call.self.IsObject() ? call.self : Value::Obj(call.interpreter.Global());
    if (!target.IsObject()) {
      return Value::Undefined();
    }
    const Value handler = Argument(call.arguments, 1);
    const bool capture = CaptureOption(Argument(call.arguments, 2));
    const std::string slot = "#on:" + js::ToString(Argument(call.arguments, 0));
    const Value* listeners = target.object->GetOwn(slot);
    if (listeners == nullptr || !listeners->IsObject()) {
      return Value::Undefined();
    }
    std::vector<Value> kept;
    for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
      const Value each = listeners->object->GetElement(i);
      // Identity, so removing works only with the same function object that
      // was added -- which is why an inline arrow cannot be removed, and is
      // the behaviour every browser has. Through the wrapper for a flagged
      // listener, so one can be removed before it fires.
      //
      // The capture flag is part of the identity, not a detail: a page that
      // registers the same function for both phases and removes one of them
      // means the one it named. Removing both would silence a handler that is
      // still registered.
      if (!js::StrictEquals(ListenerFunction(each), handler) ||
          ListenerFlag(each, kListenerCaptureSlot) != capture) {
        kept.push_back(each);
      }
    }
    listeners->object->SetElements(std::move(kept), {});
    return Value::Undefined();
  });
}


// --- The six that belong to the Window --------------------------------------
//
// `onblur`, `onerror`, `onfocus`, `onload`, `onresize` and `onscroll` on
// `<body>` and `<frameset>` are **window-reflected** (HTML §8.1.7.2.1): the
// element's handler slot *is* the Window's. `body.onload = f` sets
// `window.onload`, and `body.setAttribute('onload', …)` sets it too -- which is
// why every page that has ever written `<body onload="init()">` works at all,
// since the event fires at the window and never at the body.
//
// The accessors touch only the window. The element is a receiver they check
// and never read from, which is what lets them be one pair installed on two
// prototypes.
bool IsWindowReflectedHandlerName(std::string_view name) {
  return name == "onblur" || name == "onerror" || name == "onfocus" || name == "onload" ||
         name == "onresize" || name == "onscroll";
}

void InstallWindowReflectedHandlers(js::Interpreter& interpreter, const js::Value& prototype) {
  if (!prototype.IsObject()) {
    return;
  }
  static constexpr const char* kNames[] = {"onblur",   "onerror",  "onfocus",
                                           "onload",   "onresize", "onscroll"};
  for (const char* name : kNames) {
    const std::string slot = name;
    const Value getter = interpreter.NewNativeValue(name, [slot](NativeCall& call) -> Value {
      // **Null rather than undefined when nothing has set one.** A page tests
      // `if (window.onerror === null)` to decide whether it may install its
      // own, and undefined is a different answer to that question.
      const Value* stored = call.interpreter.Global()->GetOwn(slot);
      if (stored == nullptr || !stored->IsObject() || !stored->object->IsCallable()) {
        return Value::Null();
      }
      return *stored;
    });
    const Value setter = interpreter.NewNativeValue(name, [slot](NativeCall& call) -> Value {
      const Value given = Argument(call.arguments, 0);
      // Anything that is not callable stores null: the IDL type is
      // `EventHandler`, which is a nullable callback, so `body.onload = ""`
      // *clears* the handler rather than remembering the string.
      const bool callable = given.IsObject() && given.object->IsCallable();
      call.interpreter.Global()->Set(slot, callable ? given : Value::Null());
      return Value::Undefined();
    });
    if (getter.IsObject() && setter.IsObject()) {
      prototype.object->DefineAccessor(slot, getter.object, setter.object);
    }
  }
}

}  // namespace microbrowser::bindings
