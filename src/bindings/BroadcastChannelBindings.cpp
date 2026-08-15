// `BroadcastChannel`: every channel opened under the same name hears every
// message any of the others posts.
//
// youtube's Woffle offline store syncs its entity cache across tabs with one
// named `PERSISTENT_ENTITY_STORE_SYNC:…` -- `X_` in the minified bundle -- and
// `plI` only constructs the PES encoder after that channel and `indexedDB`
// both succeed. ADR 0038.
//
// **Document-scoped, and that is a real gap rather than an oversight.** This
// browser has one document per `DomBindings`, so "every channel with this
// name" today means every channel *this page* opened -- which is enough for
// `postMessage` to a channel of the same name in the same script, and enough
// for youtube's feature detect, but not enough for two tabs of the same site
// to hear each other. Widening it to the partition is `src/engine`'s to do,
// the same way a `storage` event would be: this module may not see a
// partition key, so the fan-out has to happen on the far side of ADR 0021's
// seam once there is a second document to fan out to.
//
// Delivery goes through `TimerQueue::QueueTask`, exactly like
// `MessagePort::postMessage` in MessageChannels.cpp and for the same reason:
// a page that uses a channel to coordinate work across a macrotask boundary
// must actually cross one. **The message is structured-cloned** for the same
// reason a `MessagePort`'s is -- a receiver must not reach into the sender's
// object graph, even though everything here is one heap.

#include <optional>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Timers.h"
#include "js/StructuredClone.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

constexpr const char* kBcNameSlot = "#bcName";
constexpr const char* kBcClosedSlot = "#bcClosed";
constexpr const char* kBroadcastChannelsKey = "#broadcastChannels";
constexpr const char* kBcOnMessageSlot = "#onmessage";

bool ClosedFlag(const Value& channel) {
  if (!channel.IsObject()) {
    return true;
  }
  const Value* found = channel.object->GetOwn(kBcClosedSlot);
  return found != nullptr && js::ToBoolean(*found);
}

std::string NameOf(const Value& channel) {
  if (!channel.IsObject()) {
    return {};
  }
  const Value* found = channel.object->GetOwn(kBcNameSlot);
  return found == nullptr ? std::string() : js::ToString(*found);
}

}  // namespace

// Every live channel this document has constructed, kept in a JavaScript
// array hung off the interfaces object -- which is already a GC root, for the
// reason `LiveSockets` in SocketBindings.cpp is: a C++ table of `js::Value`
// would be invisible to the collector.
js::Value DomBindings::LiveBroadcastChannels() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kBroadcastChannelsKey);
      existing != nullptr && existing->IsObject()) {
    return *existing;
  }
  const Value list = interpreter_->NewArrayValue({});
  if (list.IsObject()) {
    interfaces_.object->Set(kBroadcastChannelsKey, list);
  }
  return list;
}

void DomBindings::InstallBroadcastChannel() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value channel_interface = MakeInterface("BroadcastChannel", InterfaceNamed("EventTarget"));
  if (!channel_interface.IsObject()) {
    return;
  }

  const auto method = [this, &channel_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      channel_interface.object->Set(name, native);
    }
  };

  // `onmessage`/`onmessageerror` are accessors over a hidden slot, exactly
  // like `MessagePort::onmessage` in MessageChannels.cpp -- see
  // `InstallOnEventAccessor` in EventBindings.cpp for why a plain data
  // property fires a handler twice.
  InstallOnEventAccessor(channel_interface, "onmessage");
  InstallOnEventAccessor(channel_interface, "onmessageerror");

  method("postMessage", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr || !call.self.IsObject()) {
      return Value::Undefined();
    }
    if (ClosedFlag(call.self)) {
      // The specified failure: a closed channel refuses rather than swallows,
      // which is the opposite of a `MessagePort`'s close -- there the *other*
      // end may still be live and posting into a closed one is routine.
      // Here the object itself is what closed.
      return ThrowDom(call, "InvalidStateError", "BroadcastChannel is closed");
    }
    const std::optional<js::SerializedValue> serialized =
        js::StructuredSerialize(call.interpreter, Argument(call.arguments, 0));
    if (!serialized.has_value()) {
      return ThrowDom(call, "DataCloneError", "the message could not be cloned");
    }
    util::AddPerformanceCounter(util::PerfCounterId::BroadcastChannelMessagesPosted);
    owner->DeliverBroadcastMessage(call.self, *serialized);
    return Value::Undefined();
  });

  method("close", [](NativeCall& call) -> Value {
    if (call.self.IsObject()) {
      call.self.object->SetHidden(kBcClosedSlot, Value::Bool(true));
      util::AddPerformanceCounter(util::PerfCounterId::BroadcastChannelClosed);
    }
    return Value::Undefined();
  });

  DomBindings* self = this;
  const Value constructor = interpreter_->NewNativeValue(
      "BroadcastChannel", [self, channel_interface](NativeCall& call) -> Value {
        const Value channel = call.interpreter.NewObjectValue();
        if (!channel.IsObject()) {
          return Value::Undefined();
        }
        channel.object->SetPrototype(channel_interface.object);
        const std::string name = js::ToString(Argument(call.arguments, 0));
        channel.object->SetHidden(kBcNameSlot, Value::String(name));
        channel.object->SetHidden(kBcClosedSlot, Value::Bool(false));
        channel.object->Set("name", Value::String(name));
        self->InstallEventMethods(channel);
        const Value list = self->LiveBroadcastChannels();
        if (list.IsObject()) {
          list.object->PushElement(channel);
        }
        util::AddPerformanceCounter(util::PerfCounterId::BroadcastChannelsConstructed);
        return channel;
      });
  if (!constructor.IsObject()) {
    return;
  }
  // Overwrites the illegal-constructor placeholder `MakeInterface` declared
  // above, exactly the way `NodeInterfaces.cpp` leaves `Node`'s constructor
  // alone but a handful of others (`Image`) get a real one over a shared
  // prototype. Here the name and the interface are the same one, so both the
  // global binding and the prototype's back-reference are overwritten too.
  constructor.object->Set("prototype", channel_interface);
  channel_interface.object->Set("constructor", constructor);
  interpreter_->Global()->Set("BroadcastChannel", constructor);
  interpreter_->GlobalScope()->Declare("BroadcastChannel", constructor, false);
}

void DomBindings::DeliverBroadcastMessage(const js::Value& sender,
                                          const js::SerializedValue& serialized) {
  const Value list = LiveBroadcastChannels();
  if (!list.IsObject()) {
    return;
  }
  const std::string name = NameOf(sender);
  for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
    const Value target = list.object->GetElement(i);
    if (!target.IsObject() || js::StrictEquals(target, sender) || ClosedFlag(target) ||
        NameOf(target) != name) {
      continue;
    }
    DispatchBroadcastMessage(target, serialized);
  }
}

void DomBindings::DispatchBroadcastMessage(const js::Value& target,
                                           const js::SerializedValue& serialized) {
  DomBindings* self = this;
  const Value deliver = interpreter_->NewNativeValue(
      "deliver", [self, target, serialized](NativeCall& call) -> Value {
        if (ClosedFlag(target)) {
          return Value::Undefined();
        }
        // Deserialised at delivery, not at post: the value a handler sees is a
        // snapshot of what was posted, and mutating the sender's object
        // afterwards must not change what arrives.
        const Value data = js::StructuredDeserialize(*self->interpreter_, serialized);
        const Value event = call.interpreter.NewObjectValue();
        if (!event.IsObject()) {
          return Value::Undefined();
        }
        if (const Value prototype = self->InterfaceNamed("MessageEvent"); prototype.IsObject()) {
          event.object->SetPrototype(prototype.object);
        }
        event.object->Set("type", Value::String(std::string("message")));
        event.object->Set("target", target);
        event.object->Set("data", data);
        event.object->Set("origin", Value::String(std::string()));
        event.object->Set("source", Value::Null());
        util::AddPerformanceCounter(util::PerfCounterId::BroadcastChannelMessagesDelivered);
        if (const Value* handler = target.object->GetOwn(kBcOnMessageSlot);
            handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
          const js::Result outcome = call.interpreter.CallFunction(*handler, target, {event});
          if (outcome.completion == js::Completion::Throw) {
            call.interpreter.ReportUncaught(outcome.value, "broadcast channel handler");
          }
        }
        if (const Value* dispatch = target.object->Get("dispatchEvent");
            dispatch != nullptr && dispatch->IsObject()) {
          const js::Result outcome = call.interpreter.CallFunction(*dispatch, target, {event});
          if (outcome.completion == js::Completion::Throw) {
            call.interpreter.ReportUncaught(outcome.value, "broadcast channel listener");
          }
        }
        return Value::Undefined();
      });
  // `target` is a capture, and a capture is invisible to the collector --
  // see the same line in MessageChannels.cpp's `DispatchPortMessage`. Rooted
  // again as a property of the function object the timer queue keeps.
  if (deliver.IsObject()) {
    deliver.object->Set("#target", target);
    TimerQueue::QueueTask(*interpreter_, deliver);
  }
}

}  // namespace microbrowser::bindings
