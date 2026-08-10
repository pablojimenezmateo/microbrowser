// `MessageChannel` and `MessagePort`: two objects, entangled, where a message
// posted into one comes out of the other on a later turn of the loop.
//
// youtube's kevlar bundle does `TJg((new MessageChannel).port2)` and stops
// dead without it. That use is the common one and says what this has to get
// right: a channel is how a page reaches the *macrotask* queue. `Promise.then`
// and `queueMicrotask` run before the current turn ends; a message through a
// port does not, which is why schedulers are written on it.
//
// So delivery goes through `TimerQueue::QueueTask` rather than through the
// microtask queue. That is not a convenience -- a microtask here would make a
// page's scheduler starve exactly the work it was written to yield to, and it
// would do so invisibly.
//
// **The message is structured-cloned**, even though both ports are on this
// thread and in this heap. That is the specification's semantics and it is
// also the property a page relies on: the receiver may not reach into the
// sender's object graph. The same `js::StructuredSerialize` a worker message
// crosses a thread with, for the same reason.
//
// What is deliberately absent: the transfer list. `postMessage(value, [port])`
// detaches what it names, and a transfer that silently copied instead would
// leave a page holding two live views on what it believes is one -- a wrong
// answer rather than a missing feature. It throws.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
#include "js/StructuredClone.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// The other end of the channel, and whether this end has been started or
// closed. All three on the port object rather than in a C++ table, for the
// reason every other piece of per-object state here is: the collector can see
// a property and cannot see a `js::Value` in a field, and the entangled port
// keeps its partner alive exactly as long as the partner is reachable.
constexpr const char* kEntangledSlot = "#portOther";
constexpr const char* kStartedSlot = "#portStarted";
constexpr const char* kClosedSlot = "#portClosed";
// Messages posted to a port that has not been started yet. The specification
// calls this the port message queue, and it is the reason `start()` exists:
// a page constructs a channel, hands port2 somewhere that posts to it
// immediately, and only then attaches a handler.
constexpr const char* kPendingSlot = "#portPending";

bool FlagOn(const Value& port, const char* slot) {
  if (!port.IsObject()) {
    return false;
  }
  const Value* found = port.object->GetOwn(slot);
  return found != nullptr && js::ToBoolean(*found);
}

}  // namespace

void DomBindings::InstallMessageChannel() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value port_interface = MakeInterface("MessagePort", InterfaceNamed("EventTarget"));
  if (!port_interface.IsObject()) {
    return;
  }

  const auto method = [this, &port_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      port_interface.object->Set(name, native);
    }
  };

  method("postMessage", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr || !call.self.IsObject()) {
      return Value::Undefined();
    }
    // A transfer list is refused rather than ignored: see the note at the top.
    const Value transfer = Argument(call.arguments, 1);
    if (transfer.IsObject() && transfer.object->GetKind() == js::Object::Kind::Array &&
        transfer.object->ElementCount() != 0) {
      return ThrowDom(call, "DataCloneError", "transferring objects is not supported");
    }
    const std::optional<js::SerializedValue> serialized =
        js::StructuredSerialize(call.interpreter, Argument(call.arguments, 0));
    if (!serialized.has_value()) {
      return ThrowDom(call, "DataCloneError", "the message could not be cloned");
    }
    const Value* other = call.self.object->GetOwn(kEntangledSlot);
    if (other == nullptr || !other->IsObject() || FlagOn(*other, kClosedSlot) ||
        FlagOn(call.self, kClosedSlot)) {
      // A closed port swallows the message. Not an error: closing is how a
      // page tears a channel down, and the last post racing the close is the
      // normal way that happens.
      return Value::Undefined();
    }
    owner->DeliverPortMessage(*other, *serialized);
    return Value::Undefined();
  });

  method("start", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner != nullptr) {
      owner->StartPort(call.self);
    }
    return Value::Undefined();
  });

  method("close", [](NativeCall& call) -> Value {
    if (call.self.IsObject()) {
      call.self.object->SetHidden(kClosedSlot, Value::Bool(true));
    }
    return Value::Undefined();
  });

  // `onmessage` starts the port, which is the specification's rule and is the
  // reason most code never calls `start()`. A plain data property would make
  // that impossible to notice, so it is an accessor pair over a hidden slot.
  const Value get_handler =
      interpreter_->NewNativeValue("onmessage", [](NativeCall& call) -> Value {
        const Value* found =
            call.self.IsObject() ? call.self.object->GetOwn("#onmessage") : nullptr;
        return found == nullptr ? Value::Null() : *found;
      });
  const Value set_handler =
      interpreter_->NewNativeValue("onmessage", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (call.self.IsObject()) {
          call.self.object->SetHidden("#onmessage", Argument(call.arguments, 0));
        }
        if (owner != nullptr) {
          owner->StartPort(call.self);
        }
        return Value::Undefined();
      });
  if (get_handler.IsObject() && set_handler.IsObject()) {
    get_handler.object->Set(kOwnerSlot, PointerValue(this));
    set_handler.object->Set(kOwnerSlot, PointerValue(this));
    port_interface.object->DefineAccessor("onmessage", get_handler.object, set_handler.object);
  }

  DomBindings* self = this;
  const Value constructor =
      interpreter_->NewNativeValue("MessageChannel", [self, port_interface](NativeCall& call) {
        const Value channel = call.interpreter.NewObjectValue();
        const Value port1 = call.interpreter.NewObjectValue();
        const Value port2 = call.interpreter.NewObjectValue();
        if (!channel.IsObject() || !port1.IsObject() || !port2.IsObject()) {
          return Value::Undefined();
        }
        for (const Value& port : {port1, port2}) {
          port.object->SetPrototype(port_interface.object);
          port.object->SetHidden(kStartedSlot, Value::Bool(false));
          port.object->SetHidden(kClosedSlot, Value::Bool(false));
          port.object->SetHidden(kPendingSlot, call.interpreter.NewArrayValue({}));
          self->InstallEventMethods(port);
        }
        // Entangled both ways, and that is what keeps them alive: each port is
        // a property of the other, so a page that drops `channel` and keeps
        // `port2` keeps port1 too -- which it has to, because port1 is where
        // its messages come from.
        port1.object->SetHidden(kEntangledSlot, port2);
        port2.object->SetHidden(kEntangledSlot, port1);
        channel.object->Set("port1", port1);
        channel.object->Set("port2", port2);
        return channel;
      });
  if (constructor.IsObject()) {
    interpreter_->Global()->Set("MessageChannel", constructor);
    interpreter_->GlobalScope()->Declare("MessageChannel", constructor, false);
  }
}

void DomBindings::StartPort(const js::Value& port) {
  if (!port.IsObject() || FlagOn(port, kStartedSlot)) {
    return;
  }
  port.object->SetHidden(kStartedSlot, Value::Bool(true));
  // Whatever arrived before the port was started is delivered now, in order.
  // Taken off the port first, so a handler that posts back does not append to
  // the list being drained.
  const Value* pending = port.object->GetOwn(kPendingSlot);
  if (pending == nullptr || !pending->IsObject()) {
    return;
  }
  const Value queued = *pending;
  port.object->SetHidden(kPendingSlot, interpreter_->NewArrayValue({}));
  const std::size_t count = queued.object->ElementCount();
  for (std::size_t i = 0; i < count; ++i) {
    DispatchPortMessage(port, queued.object->GetElement(i));
  }
}

void DomBindings::DeliverPortMessage(const js::Value& port,
                                     const js::SerializedValue& serialized) {
  // Deserialised at *delivery* rather than now, so the value a handler sees is
  // a snapshot of what was posted -- mutating the object afterwards must not
  // change what arrives, which is the entire point of cloning it.
  const Value data = js::StructuredDeserialize(*interpreter_, serialized);
  if (!FlagOn(port, kStartedSlot)) {
    // Queued on the port rather than dropped: a page routinely posts to a port
    // before the far end has attached a handler, and `start()` is what
    // releases these.
    if (const Value* pending = port.object->GetOwn(kPendingSlot);
        pending != nullptr && pending->IsObject()) {
      pending->object->SetElement(pending->object->ElementCount(), data);
    }
    return;
  }
  DispatchPortMessage(port, data);
}

void DomBindings::DispatchPortMessage(const js::Value& port, const js::Value& data) {
  // A *task*, not a microtask. See the note at the top of this file: a page
  // uses a channel precisely to get past the end of the current turn.
  DomBindings* self = this;
  const Value deliver =
      interpreter_->NewNativeValue("deliver", [self, port, data](NativeCall& call) -> Value {
        if (FlagOn(port, kClosedSlot)) {
          return Value::Undefined();
        }
        const Value event = call.interpreter.NewObjectValue();
        if (!event.IsObject()) {
          return Value::Undefined();
        }
        // A real MessageEvent, because there is one: this is the interface a
        // page checks with `instanceof` and patches through
        // `MessageEvent.prototype`.
        if (const Value prototype = self->InterfaceNamed("MessageEvent"); prototype.IsObject()) {
          event.object->SetPrototype(prototype.object);
        }
        event.object->Set("type", Value::String(std::string("message")));
        event.object->Set("target", port);
        event.object->Set("data", data);
        // The two origin fields a page reads off a message event. Empty rather
        // than absent, and honestly so: a channel has no origin behind it --
        // both ends are this document -- and the empty string is what a
        // same-document channel reports.
        event.object->Set("origin", Value::String(std::string()));
        event.object->Set("source", Value::Null());
        event.object->Set("ports", call.interpreter.NewArrayValue({}));
        if (const Value* handler = port.object->GetOwn("#onmessage");
            handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
          const js::Result outcome = call.interpreter.CallFunction(*handler, port, {event});
          if (outcome.completion == js::Completion::Throw) {
            call.interpreter.ReportUncaught(outcome.value, "message port handler");
          }
        }
        if (const Value* dispatch = port.object->Get("dispatchEvent");
            dispatch != nullptr && dispatch->IsObject()) {
          const js::Result outcome = call.interpreter.CallFunction(*dispatch, port, {event});
          if (outcome.completion == js::Completion::Throw) {
            call.interpreter.ReportUncaught(outcome.value, "message port listener");
          }
        }
        return Value::Undefined();
      });
  // The task holds the port and the data in its captures, and the *function
  // object* is what the timer queue keeps -- in the JavaScript heap, where the
  // collector can see it. A capture alone would not be reachable.
  if (deliver.IsObject()) {
    deliver.object->Set("#port", port);
    deliver.object->Set("#data", data);
    TimerQueue::QueueTask(*interpreter_, deliver);
  }
}

}  // namespace microbrowser::bindings
