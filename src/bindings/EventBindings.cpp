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

// Where a listener with flags keeps the function it wraps. A listener is
// stored as the function itself unless it needs a flag, which keeps the common
// case one object rather than two -- and the common case is most of the 930
// registrations ADR 0017 counted.
constexpr const char* kListenerFunctionSlot = "#fn";
constexpr const char* kListenerCaptureSlot = "#capture";
constexpr const char* kListenerPassiveSlot = "#passive";
constexpr const char* kListenerOnceSlot = "#once";
// Set on the *event* while a passive listener runs. `preventDefault` reads it
// and does nothing, which is the whole point of the flag: a page promises not
// to cancel so the browser can start scrolling before the handler returns.
constexpr const char* kEventInPassiveSlot = "#inPassive";

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

// `addEventListener(type, fn, true)` and `addEventListener(type, fn, {capture:
// true})` mean the same thing, and the third argument is a boolean far more
// often than it is an object.
bool CaptureOption(const Value& options) {
  if (options.IsObject()) {
    return Option(options, "capture");
  }
  return js::ToBoolean(options);
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
    // The flags are a wrapper around the function rather than fields beside it,
    // so the list stays one array and removal stays identity on the function.
    // A listener with no flags -- which is most of them -- is stored as itself.
    const Value options = Argument(call.arguments, 2);
    const bool capture = CaptureOption(options);
    const bool passive = Option(options, "passive");
    const bool once = Option(options, "once");
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
                            "#on:" + (type == nullptr ? std::string() : js::ToString(*type)),
                            EventPhase::AtTarget);
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

// `MessagePort::onmessage` (MessageChannels.cpp) defines its handler property
// this way rather than as plain data, and every later `on<type>` property
// this codebase adds must follow it: `RunListenersOn` above reads
// `holder.object->Get("on" + type)` as an *implicit* listener on every
// dispatch (the `attribute` local), which is the specification's rule for an
// `onclick` HTML attribute. A handler stored as ordinary data is visible to
// that implicit read as well as to whatever explicit check installed it,
// so it fires twice -- once from the explicit check, once from
// `dispatchEvent`'s own attribute pass. `Object::Get` returns nullptr for an
// accessor property (Heap.cpp `Object::Get`), which is what keeps the
// implicit pass from seeing a handler stored behind one, and is why this
// helper exists rather than a plain `Set`.
void DomBindings::InstallOnEventAccessor(const js::Value& prototype, const char* name) {
  if (!prototype.IsObject()) {
    return;
  }
  const std::string slot = std::string("#") + name;
  const Value getter = interpreter_->NewNativeValue(
      name, [slot](NativeCall& call) -> Value {
        const Value* found = call.self.IsObject() ? call.self.object->GetOwn(slot) : nullptr;
        return found == nullptr ? Value::Null() : *found;
      });
  const Value setter = interpreter_->NewNativeValue(
      name, [slot](NativeCall& call) -> Value {
        if (call.self.IsObject()) {
          call.self.object->SetHidden(slot, Argument(call.arguments, 0));
        }
        return Value::Undefined();
      });
  if (getter.IsObject() && setter.IsObject()) {
    prototype.object->DefineAccessor(name, getter.object, setter.object);
  }
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
    // Looked up, **not created**. Creating it here would have to guess the
    // grandparent, and the only guess available is "none" -- so a chain built
    // out of order would silently give the middle link the root's methods and
    // no Event above it. The list in InstallEventConstructors is parent-first
    // for that reason, and this is what makes breaking that order fail loudly
    // (no prototype) rather than quietly (the wrong one).
    if (const Value* base = interfaces_.object->GetOwn(parent);
        base != nullptr && base->IsObject()) {
      prototype.object->SetPrototype(base->object);
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
            //
            // Nor does it do anything inside a passive listener. That is the
            // whole content of `{passive: true}`: the page promised not to
            // cancel, the browser is entitled to have already started, and
            // honouring the call afterwards would make the promise meaningless.
            const Value* is_cancelable = call.self.object->Get("cancelable");
            const Value* in_passive = call.self.object->GetOwn(kEventInPassiveSlot);
            const bool passive = in_passive != nullptr && js::ToBoolean(*in_passive);
            const bool allowed =
                !guard_cancelable ||
                (!passive && is_cancelable != nullptr && js::ToBoolean(*is_cancelable));
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

    // The phase constants, on the interface a page reads them from. A handler
    // that compares `e.eventPhase === Event.CAPTURING_PHASE` needs both halves
    // to exist or the comparison is against undefined and always false.
    prototype.object->Set("NONE", Value::Number(0));
    prototype.object->Set("CAPTURING_PHASE", Value::Number(1));
    prototype.object->Set("AT_TARGET", Value::Number(2));
    prototype.object->Set("BUBBLING_PHASE", Value::Number(3));
  }

  // A constructor, so the name resolves and `Event.prototype` is reachable --
  // which is what a polyfill patches and what `instanceof` needs.
  DomBindings* self = this;
  const std::string interface(name);
  const bool custom = interface == "CustomEvent";
  const bool keyboard = interface == "KeyboardEvent";
  const bool mouseish = interface == "MouseEvent" || interface == "PointerEvent" ||
                        interface == "WheelEvent" || interface == "DragEvent";
  const Value constructor =
      interpreter_->NewNativeValue(name, [self, custom, keyboard, mouseish, prototype](NativeCall& call) {
    const std::string type = js::ToString(Argument(call.arguments, 0));
    const Value options = Argument(call.arguments, 1);
    const auto option = [&options](const char* key) {
      if (!options.IsObject()) {
        return false;
      }
      const Value* found = options.object->Get(key);
      return found != nullptr && js::ToBoolean(*found);
    };
    const auto copy_string = [&options](js::Object& into, const char* key, const char* fallback = "") {
      if (!options.IsObject()) {
        into.Set(key, Value::String(fallback));
        return;
      }
      const Value* found = options.object->Get(key);
      into.Set(key, Value::String(found == nullptr ? fallback : js::ToString(*found)));
    };
    const auto copy_number = [&options](js::Object& into, const char* key, double fallback = 0.0) {
      if (!options.IsObject()) {
        into.Set(key, Value::Number(fallback));
        return;
      }
      const Value* found = options.object->Get(key);
      into.Set(key, Value::Number(found == nullptr ? fallback : js::ToNumber(*found)));
    };
    // Untrusted: a page made it. Returning the object is what makes this work
    // under `new` -- the receiver a construct call builds is discarded in
    // favour of an object the native returns.
    const Value event = self->MakeEvent(type, option("bubbles"), option("cancelable"), false);
    // **Its own prototype, not Event's.** MakeEvent gives every event
    // `Event.prototype`, because that is right for the ones the browser makes
    // and hands to a listener -- but a constructed one has to be an instance of
    // the constructor that was called. Without this line the whole hierarchy
    // above existed and nothing was ever an instance of any of it:
    // `new CustomEvent('x') instanceof CustomEvent` was false, which is the
    // check a page makes before reading `.detail`.
    if (event.IsObject() && prototype.IsObject()) {
      event.object->SetPrototype(prototype.object);
    }
    if (event.IsObject() && custom) {
      const Value* detail = options.IsObject() ? options.object->Get("detail") : nullptr;
      event.object->Set("detail", detail == nullptr ? Value::Undefined() : *detail);
    }
    // KeyboardEventInit / MouseEventInit. Without these, `new KeyboardEvent(
    // 'keydown', { key: 'Enter', code: 'Enter' })` produced an event whose
    // `.key` and `.code` were undefined — and a handler that branches on them
    // took the wrong arm (TD-0026). Constructed events are untrusted; legacy
    // `keyCode`/`which` are whatever the page passed, else 0 (the platform
    // value for a synthesised event).
    if (event.IsObject() && keyboard) {
      copy_string(*event.object, "key");
      copy_string(*event.object, "code");
      copy_number(*event.object, "keyCode");
      copy_number(*event.object, "which");
      copy_number(*event.object, "location");
      copy_number(*event.object, "charCode");
      event.object->Set("repeat", Value::Bool(option("repeat")));
      event.object->Set("ctrlKey", Value::Bool(option("ctrlKey")));
      event.object->Set("shiftKey", Value::Bool(option("shiftKey")));
      event.object->Set("altKey", Value::Bool(option("altKey")));
      event.object->Set("metaKey", Value::Bool(option("metaKey")));
    }
    if (event.IsObject() && mouseish) {
      copy_number(*event.object, "clientX");
      copy_number(*event.object, "clientY");
      copy_number(*event.object, "screenX");
      copy_number(*event.object, "screenY");
      copy_number(*event.object, "offsetX");
      copy_number(*event.object, "offsetY");
      copy_number(*event.object, "pageX");
      copy_number(*event.object, "pageY");
      copy_number(*event.object, "button");
      copy_number(*event.object, "buttons");
      copy_number(*event.object, "movementX");
      copy_number(*event.object, "movementY");
      event.object->Set("ctrlKey", Value::Bool(option("ctrlKey")));
      event.object->Set("shiftKey", Value::Bool(option("shiftKey")));
      event.object->Set("altKey", Value::Bool(option("altKey")));
      event.object->Set("metaKey", Value::Bool(option("metaKey")));
    }
    return event;
  });
  if (constructor.IsObject()) {
    constructor.object->Set(kOwnerSlot, PointerValue(this));
    constructor.object->Set("prototype", prototype);
    // On the constructor as well as the prototype, because that is where a page
    // spells them: `Event.CAPTURING_PHASE`, not `Event.prototype.…`.
    for (const char* constant_name : {"NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"}) {
      if (const Value* constant = prototype.object->Get(constant_name)) {
        constructor.object->Set(constant_name, *constant);
      }
    }
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
  event.object->Set("eventPhase", Value::Number(static_cast<double>(EventPhase::None)));
  // Whether the *browser* made this event or a page did. A page's own event
  // must never be able to cause what a real one causes -- see DispatchClick --
  // and the flag is how anything downstream can tell without having to know
  // where it came from.
  //
  // A getter with no setter, and set from the constructor that was used rather
  // than assigned afterwards. ADR 0017 §3: there must be no way to make it
  // true, because the gates that will eventually read it -- opening a window,
  // entering fullscreen, reading the clipboard -- are reading it as a statement
  // about the user. Assigning to a getter-only property is a silent no-op in
  // this engine, so `event.isTrusted = true` leaves it false.
  const Value getter = TrustedGetter(trusted);
  if (getter.IsObject()) {
    event.object->DefineAccessor("isTrusted", getter.object, nullptr);
  }
  return event;
}

js::Value DomBindings::TrustedGetter(bool trusted) {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  // Two functions for the whole process, not one per event. They close over
  // nothing and read nothing: what an event's `isTrusted` answers is decided
  // by which of the two it was given, and there is no slot on the event for a
  // page to reach for.
  const char* name = trusted ? "#isTrustedTrue" : "#isTrustedFalse";
  if (const Value* existing = interfaces_.object->GetOwn(name)) {
    return *existing;
  }
  const Value native = interpreter_->NewNativeValue(
      "isTrusted", [trusted](NativeCall&) { return Value::Bool(trusted); });
  if (native.IsObject()) {
    interfaces_.object->Set(name, native);
  }
  return native;
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
  //
  // **Parent-first, and the order is load-bearing** -- see EventPrototype,
  // which looks a parent up rather than creating one, because creating it
  // would have to guess the grandparent.
  EventPrototype("Event", nullptr);
  EventPrototype("CustomEvent", "Event");
  // UIEvent is the real base of everything a user does, and it was missing:
  // MouseEvent and KeyboardEvent chained straight to Event, so a polyfill that
  // patches `UIEvent.prototype` -- which is where a library puts a fix meant
  // for every input event at once -- reached nothing.
  EventPrototype("UIEvent", "Event");
  EventPrototype("MouseEvent", "UIEvent");
  // `e instanceof KeyboardEvent` is how a handler bound to several event types
  // tells a key from a click, and it is the check a page makes before reading
  // `e.key` off something that might not have one.
  EventPrototype("KeyboardEvent", "UIEvent");
  // FocusEvent was being created on demand by the focus dispatcher with `Event`
  // as its parent, which is one place deciding the hierarchy for itself. It is
  // declared here with the rest so there is one list.
  EventPrototype("FocusEvent", "UIEvent");
  EventPrototype("InputEvent", "UIEvent");
  // The three that extend MouseEvent, and the reason the chain matters: a
  // handler bound to both `click` and `wheel` reads `e.clientX` off either, and
  // that only works if a WheelEvent *is* a MouseEvent.
  //
  // The list is what youtube's bundle names, which is how every list in this
  // file is chosen: `WheelEvent`, `PointerEvent`, `DragEvent`, `MessageEvent`
  // and `PromiseRejectionEvent` all appear in it, and `WheelEvent` is where it
  // stopped at source offset 9,444,946 -- 88% of the way through 10.7MB.
  EventPrototype("WheelEvent", "MouseEvent");
  EventPrototype("PointerEvent", "MouseEvent");
  EventPrototype("DragEvent", "MouseEvent");
  // `MessageEvent` is the one this browser actually delivers: a `MessagePort`
  // hands one to its listener, so this is the interface behind an object that
  // already exists rather than a name in front of nothing.
  EventPrototype("MessageEvent", "Event");
  // `HashChangeEvent`, which this browser really dispatches: a same-document
  // navigation that only moves the fragment fires one at the window, and until
  // the interface existed that event was an `Event` with two extra properties
  // on it -- so `e instanceof HashChangeEvent` was a ReferenceError.
  EventPrototype("HashChangeEvent", "Event");
  // `ProgressEvent`, which is what an XHR's `progress`/`load`/`error` are.
  EventPrototype("ProgressEvent", "Event");
  // `PromiseRejectionEvent`. Nothing dispatches one -- an unhandled rejection
  // gets a console line and no more, which docs/js-conformance-roadmap.md
  // records -- so this is the constructor and the prototype and no behaviour,
  // which is what a page that *constructs* one uses.
  EventPrototype("PromiseRejectionEvent", "Event");
  // `ErrorEvent`, which is the one a page constructs itself rather than
  // receives: the shape is `new ErrorEvent("error", {message, filename, lineno,
  // colno})`, dispatched at the window so that an error a library caught still
  // reaches whatever is listening for one. Nothing in this browser dispatches an
  // ErrorEvent yet -- a script that throws gets a console line -- so this is the
  // constructor and the prototype and no more, which is what the page uses.
  EventPrototype("ErrorEvent", "Event");

  // `DOMException`, which is the type every web API throws and is therefore not
  // an event constructor at all. It is installed from here because this is
  // where the page's globals are declared and it must exist before anything
  // that can throw one; the type itself lives in DomExceptions.cpp with the
  // WebIDL error-names table it needs.
  InstallDomException(*interpreter_);
}

const char* DomBindings::LegacyEventInterface(std::string_view name) {
  // The DOM's `createEvent` table, which is a *closed* list of legacy names
  // matched ASCII-case-insensitively -- `document.createEvent("mouseevents")`
  // is the spelling half the pages that use this API were written with.
  //
  // Case-insensitive over ASCII only, and that is not a detail: the test suite
  // asks for `"UİEvent"` and `"UıEvent"` -- the Turkish dotted and
  // dotless i -- and both must be *unrecognised*. A locale-aware fold makes
  // one of them "uievent" and hands the page an interface it never named.
  //
  // Names this browser has no interface behind are absent from the table
  // rather than mapped to `Event`. `document.createEvent("DeviceMotionEvent")`
  // throwing NotSupportedError is the answer a browser with no motion sensor
  // gives; handing back an Event with the wrong prototype would tell a page
  // the feature is there, which is ADR 0012's rule at the point it matters --
  // and for the sensor events it would also be a fingerprinting surface
  // (ADR 0029) opened by a table entry.
  struct Alias {
    std::string_view name;
    const char* interface;
  };
  static constexpr Alias kAliases[] = {
      {"event", "Event"},
      {"events", "Event"},
      {"htmlevents", "Event"},
      {"svgevents", "Event"},
      {"customevent", "CustomEvent"},
      {"uievent", "UIEvent"},
      {"uievents", "UIEvent"},
      {"mouseevent", "MouseEvent"},
      {"mouseevents", "MouseEvent"},
      {"keyboardevent", "KeyboardEvent"},
      {"focusevent", "FocusEvent"},
      {"dragevent", "DragEvent"},
      {"messageevent", "MessageEvent"},
      {"hashchangeevent", "HashChangeEvent"},
  };
  const std::string folded = util::AsciiLowerCase(name);
  for (const Alias& alias : kAliases) {
    if (alias.name == folded) {
      return alias.interface;
    }
  }
  return nullptr;
}

js::Value DomBindings::CreateLegacyEvent(const char* interface) {
  // `document.createEvent('Event')` makes an *uninitialised* event: it has no
  // type until `initEvent` is called, and dispatching it before that does
  // nothing. That two-step shape is the whole reason this exists separately
  // from the constructors -- it is the API a polyfill written for IE uses, and
  // it is where youtube.com's web components polyfill stopped.
  const Value event = MakeEvent("", false, false, false);
  if (!event.IsObject()) {
    return Value::Undefined();
  }
  // The prototype the *named* interface has, not Event's. `createEvent` is
  // documented by its return type, and a page that asks for a MouseEvent and
  // gets something that is not one takes the branch written for browsers that
  // never had the API.
  if (interface != nullptr) {
    if (const Value* prototype = interfaces_.IsObject() ? interfaces_.object->GetOwn(interface)
                                                        : nullptr;
        prototype != nullptr && prototype->IsObject()) {
      event.object->SetPrototype(prototype->object);
    }
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
