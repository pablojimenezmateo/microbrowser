#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
    if (!call.self.IsObject()) {
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
    const Value* existing = call.self.object->GetOwn(slot);
    if (existing != nullptr && existing->IsObject()) {
      existing->object->PushElement(handler);
      return Value::Undefined();
    }
    call.self.object->Set(slot, call.interpreter.NewArrayValue({handler}));
    return Value::Undefined();
  });
  // A page dispatching its own event. Untrusted by construction: the event
  // still runs every listener, and nothing in the engine acts on it -- a
  // synthetic click does not follow a link.
  method("dispatchEvent", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    const Value event = Argument(call.arguments, 0);
    if (owner == nullptr || self == nullptr || !event.IsObject()) {
      return call.Throw("TypeError", "dispatchEvent needs an event");
    }
    const bool prevented = owner->DispatchEventTo(*self, event);
    // True when nothing called preventDefault, which is the inverse of what
    // dispatch reports and is what the specification returns.
    return Value::Bool(!prevented);
  });
  method("removeEventListener", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    const Value handler = Argument(call.arguments, 1);
    const std::string slot = "#on:" + js::ToString(Argument(call.arguments, 0));
    const Value* listeners = call.self.object->GetOwn(slot);
    if (listeners == nullptr || !listeners->IsObject()) {
      return Value::Undefined();
    }
    std::vector<Value> kept;
    for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
      const Value each = listeners->object->GetElement(i);
      // Identity, so removing works only with the same function object that
      // was added -- which is why an inline arrow cannot be removed, and is
      // the behaviour every browser has.
      if (!js::StrictEquals(each, handler)) {
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

  for (dom::Node* node : chain) {
    const Value wrapper = WrapperFor(node);
    if (!wrapper.IsObject()) {
      continue;
    }
    const Value* listeners = wrapper.object->GetOwn(slot);
    if (listeners == nullptr || !listeners->IsObject()) {
      continue;
    }
    event.object->Set("currentTarget", wrapper);
    // A copy, because a handler is allowed to add or remove listeners and the
    // set that runs is the set that existed when the event was dispatched.
    std::vector<Value> handlers;
    for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
      handlers.push_back(listeners->object->GetElement(i));
    }
    for (const Value& handler : handlers) {
      // `this` is the node the listener was registered on, which is what a
      // handler written as an ordinary function expects.
      (void)interpreter_->CallFunction(handler, wrapper, {event});
      const Value* immediate = event.object->GetOwn("#stopImmediate");
      if (immediate != nullptr && js::ToBoolean(*immediate)) {
        break;
      }
    }
    const Value* stopped = event.object->GetOwn("cancelBubble");
    if (stopped != nullptr && js::ToBoolean(*stopped)) {
      break;
    }
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
