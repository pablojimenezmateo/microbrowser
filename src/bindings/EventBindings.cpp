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

bool DomBindings::DispatchClick(dom::Element& target) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // The event object, made once and passed to every handler on the way up:
  // `defaultPrevented` has to survive the walk, or a handler on the target
  // could not stop a link its ancestor would otherwise follow.
  const Value event = interpreter_->NewObjectValue();
  if (!event.IsObject()) {
    return false;
  }
  event.object->Set("type", Value::String(std::string("click")));
  event.object->Set("target", WrapperFor(&target));
  event.object->Set("defaultPrevented", Value::Bool(false));
  event.object->Set("cancelBubble", Value::Bool(false));
  const auto flag = [this, &event](const char* name, const char* field) {
    const Value native = interpreter_->NewNativeValue(name, [field](NativeCall& call) {
      if (call.self.IsObject()) {
        call.self.object->Set(field, Value::Bool(true));
      }
      return Value::Undefined();
    });
    if (native.IsObject()) {
      event.object->Set(name, native);
    }
  };
  flag("preventDefault", "defaultPrevented");
  flag("stopPropagation", "cancelBubble");

  // From the target up to the root, which is what bubbling is. The ancestor
  // chain is read before any handler runs: a handler that reparents the target
  // must not change which ancestors see the event.
  std::vector<dom::Node*> chain;
  for (dom::Node* walk = &target; walk != nullptr; walk = walk->Parent()) {
    chain.push_back(walk);
  }

  for (dom::Node* node : chain) {
    const Value wrapper = WrapperFor(node);
    if (!wrapper.IsObject()) {
      continue;
    }
    const Value* listeners = wrapper.object->GetOwn("#on:click");
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
    }
    const Value* stopped = event.object->GetOwn("cancelBubble");
    if (stopped != nullptr && js::ToBoolean(*stopped)) {
      break;
    }
  }
  // Handlers run as a turn of their own, so anything they queued settles
  // before the click is over -- the same rule a script gets.
  interpreter_->DrainMicrotasks();

  const Value* prevented = event.object->GetOwn("defaultPrevented");
  return prevented != nullptr && js::ToBoolean(*prevented);
}

}  // namespace microbrowser::bindings
