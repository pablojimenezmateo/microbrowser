#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"

// Event listeners and click dispatch.
//
// Split from DomBindings.cpp because that file reached the module's line cap,
// and the cap is written to mean a missing translation unit rather than a
// bigger file. Events are their own concern: the rest of the module answers
// questions about the tree, and this part is the one place where the *browser*
// calls into a page rather than the other way round.
//
// Dispatch is a C++ entry point with no script-facing counterpart, and that is
// deliberate. The only thing allowed to say a click happened is the thing that
// saw one; a page that could dispatch its own trusted events could make a form
// submit itself.

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

// Where a `{once: true}` listener keeps the function it wraps. A listener is
// stored as the function itself unless it needs a flag, which keeps the common
// case one object rather than two.
constexpr const char* kListenerFunctionSlot = "#fn";

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

bool ListenerIsOnce(const Value& entry) {
  return entry.IsObject() && !entry.object->IsCallable();
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

// Removes one listener entry from a live list, by identity of the entry.
void ForgetListener(const Value& listeners, const Value& entry) {
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

void DomBindings::InstallEventMethods(const js::Value& wrapper) {
  const auto method = [this, &wrapper](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      wrapper.object->Set(name, native);
    }
  };
  // Listeners live on the wrapper, keyed by type, so they are collected with
  // it and cannot outlive the node they were registered on.
  method("addEventListener", [](NativeCall& call) {
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
    if (!handler.IsObject() || !handler.object->IsCallable()) {
      // A non-callable listener is ignored rather than refused, which is what
      // the specification says and what stops a page's optional callback from
      // being fatal.
      return Value::Undefined();
    }
    const std::string slot = "#on:" + js::ToString(Argument(call.arguments, 0));
    // `{once: true}` is a wrapper around the function rather than a flag beside
    // it, so the list stays one array and removal stays identity on the
    // function. `capture` and `passive` are ADR 0017's, and are deliberately
    // not read here: accepting an option and ignoring it is the stub problem.
    Value entry = handler;
    if (Option(Argument(call.arguments, 2), "once")) {
      const Value once = call.interpreter.NewObjectValue();
      if (once.IsObject()) {
        once.object->Set(kListenerFunctionSlot, handler);
        entry = once;
      }
    }
    const Value* existing = target.object->GetOwn(slot);
    if (existing != nullptr && existing->IsObject()) {
      existing->object->PushElement(entry);
      return Value::Undefined();
    }
    target.object->Set(slot, call.interpreter.NewArrayValue({entry}));
    return Value::Undefined();
  });
  // A page dispatching its own event. Untrusted by construction: the event
  // still runs every listener, and nothing in the engine acts on it -- a
  // synthetic click does not follow a link.
  method("dispatchEvent", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    const Value event = Argument(call.arguments, 0);
    if (owner == nullptr || !event.IsObject()) {
      return call.Throw("TypeError", "dispatchEvent needs an event");
    }
    if (self == nullptr) {
      // A target that is not a node -- `window`. There is no tree to walk, so
      // the event reaches its listeners and stops.
      const Value* type = event.object->GetOwn("type");
      event.object->Set("target", call.self);
      owner->RunListenersOn(call.self, event,
                            "#on:" + (type == nullptr ? std::string() : js::ToString(*type)));
      const Value* prevented_here = event.object->GetOwn("defaultPrevented");
      return Value::Bool(prevented_here == nullptr || !js::ToBoolean(*prevented_here));
    }
    const bool prevented = owner->DispatchEventTo(*self, event);
    // True when nothing called preventDefault, which is the inverse of what
    // dispatch reports and is what the specification returns.
    return Value::Bool(!prevented);
  });
  method("removeEventListener", [](NativeCall& call) {
    const Value target =
        call.self.IsObject() ? call.self : Value::Obj(call.interpreter.Global());
    if (!target.IsObject()) {
      return Value::Undefined();
    }
    const Value handler = Argument(call.arguments, 1);
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
      // the behaviour every browser has. Through the wrapper for a `once`
      // listener, so that one can be removed before it fires.
      if (!js::StrictEquals(ListenerFunction(each), handler)) {
        kept.push_back(each);
      }
    }
    listeners->object->SetElements(std::move(kept), {});
    return Value::Undefined();
  });

}

js::Value DomBindings::EventPrototype(const char* name, const char* parent) {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(name)) {
    return *existing;
  }
  const Value prototype = interpreter_->NewObjectValue();
  if (!prototype.IsObject()) {
    return Value::Undefined();
  }
  if (parent != nullptr) {
    const Value base = EventPrototype(parent, nullptr);
    if (base.IsObject()) {
      prototype.object->SetPrototype(base.object);
    }
  }
  interfaces_.object->Set(name, prototype);

  if (parent == nullptr) {
    // The three ways to stop an event, installed once here rather than on
    // every event object -- the same change the node wrappers got, and for the
    // same two reasons: a native function per event is a cost per dispatch,
    // and a page reads `Event.prototype` to patch it.
    const auto flag = [this, &prototype](const char* method, const char* field,
                                         bool guard_cancelable, bool immediate) {
      const Value native = interpreter_->NewNativeValue(
          method, [field, guard_cancelable, immediate](NativeCall& call) {
            if (!call.self.IsObject()) {
              return Value::Undefined();
            }
            // `preventDefault` on an event that is not cancelable does
            // nothing, which is the rule and is what keeps a handler from
            // believing it stopped something it did not.
            const Value* is_cancelable = call.self.object->Get("cancelable");
            const bool allowed =
                !guard_cancelable || (is_cancelable != nullptr && js::ToBoolean(*is_cancelable));
            if (allowed) {
              call.self.object->Set(field, Value::Bool(true));
            }
            if (immediate) {
              // Not the same as stopPropagation, and pages rely on the
              // difference: this one also stops the remaining listeners on the
              // *current* node, which the dispatch loop reads between handlers.
              call.self.object->Set("#stopImmediate", Value::Bool(true));
            }
            return Value::Undefined();
          });
      if (native.IsObject()) {
        prototype.object->Set(method, native);
      }
    };
    flag("preventDefault", "defaultPrevented", true, false);
    flag("stopPropagation", "cancelBubble", false, false);
    flag("stopImmediatePropagation", "cancelBubble", false, true);
  }

  // A constructor, so the name resolves and `Event.prototype` is reachable --
  // which is what a polyfill patches and what `instanceof` needs.
  DomBindings* self = this;
  const bool custom = std::string(name) == "CustomEvent";
  const Value constructor = interpreter_->NewNativeValue(name, [self, custom](NativeCall& call) {
    const std::string type = js::ToString(Argument(call.arguments, 0));
    const Value options = Argument(call.arguments, 1);
    const auto option = [&options](const char* key) {
      if (!options.IsObject()) {
        return false;
      }
      const Value* found = options.object->Get(key);
      return found != nullptr && js::ToBoolean(*found);
    };
    // Untrusted: a page made it. Returning the object is what makes this work
    // under `new` -- the receiver a construct call builds is discarded in
    // favour of an object the native returns.
    const Value event = self->MakeEvent(type, option("bubbles"), option("cancelable"), false);
    if (event.IsObject() && custom) {
      const Value* detail = options.IsObject() ? options.object->Get("detail") : nullptr;
      event.object->Set("detail", detail == nullptr ? Value::Undefined() : *detail);
    }
    return event;
  });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    interpreter_->Global()->Set(name, constructor);
    interpreter_->GlobalScope()->Declare(name, constructor, false);
  }
  return prototype;
}

js::Value DomBindings::MakeEvent(const std::string& type, bool bubbles, bool cancelable,
                                 bool trusted) {
  const Value event = interpreter_->NewObjectValue();
  if (!event.IsObject()) {
    return Value::Undefined();
  }
  const Value prototype = EventPrototype("Event", nullptr);
  if (prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  event.object->Set("type", Value::String(type));
  event.object->Set("bubbles", Value::Bool(bubbles));
  event.object->Set("cancelable", Value::Bool(cancelable));
  event.object->Set("defaultPrevented", Value::Bool(false));
  event.object->Set("cancelBubble", Value::Bool(false));
  event.object->Set("target", Value::Null());
  event.object->Set("currentTarget", Value::Null());
  // Whether the *browser* made this event or a page did. A page's own event
  // must never be able to cause what a real one causes -- see DispatchClick --
  // and the flag is how anything downstream can tell without having to know
  // where it came from.
  event.object->Set("isTrusted", Value::Bool(trusted));
  return event;
}

bool DomBindings::RunListenersOn(const js::Value& holder, const js::Value& event,
                                 const std::string& slot) {
  if (!holder.IsObject() || !event.IsObject()) {
    return false;
  }
  const Value* type = event.object->GetOwn("type");
  const Value* listeners = holder.object->GetOwn(slot);
  // `el.onsubmit = fn` is a property, not a listener, and a page sets one as
  // readily as it calls addEventListener -- reddit's challenge writes
  // `form.onsubmit = …` and would otherwise have its handler never run. The
  // property runs before the registered listeners, which is an approximation:
  // the specification registers it in the position it was first assigned.
  const Value* attribute = type == nullptr ? nullptr : holder.object->Get("on" + js::ToString(*type));
  if ((listeners == nullptr || !listeners->IsObject()) &&
      (attribute == nullptr || !attribute->IsObject() || !attribute->object->IsCallable())) {
    return false;
  }
  event.object->Set("currentTarget", holder);

  if (attribute != nullptr && attribute->IsObject() && attribute->object->IsCallable()) {
    const js::Result answer = interpreter_->CallFunction(*attribute, holder, {event});
    // The legacy cancellation: an event handler *attribute* that returns false
    // has prevented the default. A listener returning false has not, which is
    // why this lives here and not in the loop below.
    if (answer.completion != js::Completion::Throw && answer.value.type == js::ValueType::Boolean &&
        !answer.value.boolean) {
      const Value* cancelable = event.object->Get("cancelable");
      if (cancelable != nullptr && js::ToBoolean(*cancelable)) {
        event.object->Set("defaultPrevented", Value::Bool(true));
      }
    }
  }
  if (listeners == nullptr || !listeners->IsObject()) {
    const Value* stopped_here = event.object->GetOwn("cancelBubble");
    return stopped_here != nullptr && js::ToBoolean(*stopped_here);
  }

  // A copy, because a handler is allowed to add or remove listeners and the
  // set that runs is the set that existed when the event was dispatched.
  std::vector<Value> handlers;
  for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
    handlers.push_back(listeners->object->GetElement(i));
  }
  for (const Value& entry : handlers) {
    // Removed before it is called, not after: a `once` listener that dispatches
    // the same event again must not see itself still registered.
    if (ListenerIsOnce(entry)) {
      ForgetListener(*listeners, entry);
    }
    // `this` is the object the listener was registered on, which is what a
    // handler written as an ordinary function expects.
    (void)interpreter_->CallFunction(ListenerFunction(entry), holder, {event});
    const Value* immediate = event.object->GetOwn("#stopImmediate");
    if (immediate != nullptr && js::ToBoolean(*immediate)) {
      break;
    }
  }
  const Value* stopped = event.object->GetOwn("cancelBubble");
  return stopped != nullptr && js::ToBoolean(*stopped);
}

bool DomBindings::DispatchEventTo(dom::Node& target, const js::Value& event) {
  if (interpreter_ == nullptr || !event.IsObject()) {
    return false;
  }
  const Value* type_value = event.object->GetOwn("type");
  if (type_value == nullptr) {
    return false;
  }
  const std::string slot = "#on:" + js::ToString(*type_value);
  const Value* bubbles = event.object->GetOwn("bubbles");
  const bool propagates = bubbles != nullptr && js::ToBoolean(*bubbles);

  event.object->Set("target", WrapperFor(&target));

  // From the target up to the root, which is what bubbling is. The ancestor
  // chain is read before any handler runs: a handler that reparents the target
  // must not change which ancestors see the event.
  std::vector<dom::Node*> chain;
  for (dom::Node* walk = &target; walk != nullptr; walk = walk->Parent()) {
    chain.push_back(walk);
    if (!propagates) {
      break;  // a non-bubbling event reaches its target and stops
    }
  }

  bool stopped = false;
  for (dom::Node* node : chain) {
    stopped = RunListenersOn(WrapperFor(node), event, slot);
    if (stopped) {
      break;
    }
  }
  // Then the window, which is the last thing a bubbling event reaches. A page
  // that listens for `resize` or `load` listens there and nowhere else, and
  // one that listens for a click on `window` expects to see clicks on the
  // document.
  if (!stopped && propagates) {
    RunListenersOn(Value::Obj(interpreter_->Global()), event, slot);
  }
  // Handlers run as a turn of their own, so anything they queued settles
  // before the event is over -- the same rule a script gets.
  interpreter_->DrainMicrotasks();

  const Value* prevented = event.object->GetOwn("defaultPrevented");
  return prevented != nullptr && js::ToBoolean(*prevented);
}

bool DomBindings::DispatchClick(dom::Element& target) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Trusted, because the only caller is the thing that saw a real click. A
  // page's own `dispatchEvent` produces an untrusted one, which is why this
  // stayed a C++ entry point with no script-facing counterpart: an event a
  // page can forge must not be able to make a form submit itself.
  const Value event = MakeEvent("click", true, true, true);
  if (!event.IsObject()) {
    return false;
  }
  // A click is a MouseEvent, and a page tests for that: `e instanceof
  // MouseEvent` is how a handler tells a pointer event from a keyboard one.
  const Value mouse = EventPrototype("MouseEvent", "Event");
  if (mouse.IsObject()) {
    event.object->SetPrototype(mouse.object);
  }
  return DispatchEventTo(target, event);
}


bool DomBindings::DispatchSubmit(dom::Element& form) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Bubbles and is cancelable, which is the whole point: a page's `onsubmit`
  // adds the fields it wants and a `preventDefault` stops the navigation, the
  // same contract clicks already had.
  const Value event = MakeEvent("submit", true, true, true);
  if (!event.IsObject()) {
    return false;
  }
  return DispatchEventTo(form, event);
}

bool DomBindings::DispatchAtWindow(const char* type) {
  if (interpreter_ == nullptr) {
    return false;
  }
  const Value window = Value::Obj(interpreter_->Global());
  const std::string slot = std::string("#on:") + type;
  const bool listening =
      window.object->GetOwn(slot) != nullptr || window.object->Get(std::string("on") + type) != nullptr;
  if (!listening) {
    // Asked before the event is built, so a page that is listening for nothing
    // costs nothing -- which is what keeps `load` from relaying out every
    // document that ever finished loading.
    return false;
  }
  const Value event = MakeEvent(type, false, false, true);
  if (!event.IsObject()) {
    return false;
  }
  event.object->Set("target", window);
  RunListenersOn(window, event, slot);
  interpreter_->DrainMicrotasks();
  return true;
}

bool DomBindings::NotifyDomContentLoaded() {
  if (interpreter_ == nullptr) {
    return false;
  }
  // The state changes before the event fires. A handler that reads
  // `document.readyState` must not be told the parse is still running.
  SetReadyState("interactive");
  util::AddPerformanceCounter(util::PerfCounterId::EngineDomContentLoaded);
  const Value event = MakeEvent("DOMContentLoaded", true, false, true);
  if (!event.IsObject() || document_ == nullptr) {
    return false;
  }
  // Bubbles, so a listener on `window` sees it -- which is where the other
  // half of pages put it.
  DispatchEventTo(*document_, event);
  return true;
}

bool DomBindings::NotifyLoad() {
  if (interpreter_ == nullptr) {
    return false;
  }
  SetReadyState("complete");
  // At the window, not at the document: `load` does not bubble, and
  // `window.onload` is where every page listens for it.
  const bool heard = DispatchAtWindow("load");
  if (heard) {
    util::AddPerformanceCounter(util::PerfCounterId::EngineLoadEvents);
  }
  return heard;
}

void DomBindings::InstallWindowEvents() {
  // `window` *is* the global object here, so the listener methods land on it
  // directly -- which also makes `globalThis.addEventListener` the same
  // function, as it is in a browser.
  //
  // addEventListener and removeEventListener already work on any object: they
  // keep their handlers in a `#on:` slot on the receiver and never ask whether
  // it is a node. Only dispatch had to learn that a target need not be one.
  InstallEventMethods(Value::Obj(interpreter_->Global()));
}

void DomBindings::InstallEventConstructors() {
  // Naming them is what builds them. CustomEvent and MouseEvent both extend
  // Event, which is a chain a polyfill walks: it reads `Event.prototype` to
  // patch behaviour and expects the others to inherit the patch.
  EventPrototype("Event", nullptr);
  EventPrototype("CustomEvent", "Event");
  EventPrototype("MouseEvent", "Event");
}

js::Value DomBindings::CreateLegacyEvent() {
  // `document.createEvent('Event')` makes an *uninitialised* event: it has no
  // type until `initEvent` is called, and dispatching it before that does
  // nothing. That two-step shape is the whole reason this exists separately
  // from the constructors -- it is the API a polyfill written for IE uses, and
  // it is where youtube.com's web components polyfill stopped.
  const Value event = MakeEvent("", false, false, false);
  if (!event.IsObject()) {
    return Value::Undefined();
  }
  const Value init = interpreter_->NewNativeValue("initEvent", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    call.self.object->Set("type", Value::String(js::ToString(Argument(call.arguments, 0))));
    call.self.object->Set("bubbles", Value::Bool(js::ToBoolean(Argument(call.arguments, 1))));
    call.self.object->Set("cancelable", Value::Bool(js::ToBoolean(Argument(call.arguments, 2))));
    return Value::Undefined();
  });
  if (init.IsObject()) {
    event.object->Set("initEvent", init);
    // The CustomEvent spelling of the same call, which a polyfill picks
    // between by feature test.
    const Value init_custom =
        interpreter_->NewNativeValue("initCustomEvent", [](NativeCall& call) {
          if (!call.self.IsObject()) {
            return Value::Undefined();
          }
          call.self.object->Set("type", Value::String(js::ToString(Argument(call.arguments, 0))));
          call.self.object->Set("bubbles", Value::Bool(js::ToBoolean(Argument(call.arguments, 1))));
          call.self.object->Set("cancelable",
                                Value::Bool(js::ToBoolean(Argument(call.arguments, 2))));
          call.self.object->Set("detail", Argument(call.arguments, 3));
          return Value::Undefined();
        });
    if (init_custom.IsObject()) {
      event.object->Set("initCustomEvent", init_custom);
    }
  }
  return event;
}

}  // namespace microbrowser::bindings
