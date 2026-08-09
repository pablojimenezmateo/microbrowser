// `WebSocket`, as a page sees it.
//
// ADR 0020 §5. Everything a *policy* decides -- the scheme, `connect-src`, the privacy
// verdict -- is on the far side of `bindings::SocketSource`, because this module may see
// none of `net`, `csp` or `url`. What is here is the object's shape and its four events.
//
// The socket a page holds is a JavaScript object with an id on it, and the engine's table
// is keyed by that id. **The live sockets are kept in a JavaScript array hung off the
// interfaces object**, which is already a GC root: a C++ table of `js::Value` would be
// invisible to the collector, and that is the bug this module has had once.

#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Sockets.h"
#include "js/Interpreter.h"
#include "js/Value.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

constexpr const char* kSocketsKey = "#sockets";
constexpr const char* kSocketIdSlot = "#socket-id";

// The four `readyState` values, which are part of the API a page switches on.
constexpr double kConnecting = 0;
constexpr double kOpen = 1;
constexpr double kClosing = 2;
constexpr double kClosed = 3;

std::uint64_t IdOf(const Value& socket) {
  if (!socket.IsObject()) {
    return 0;
  }
  const Value* slot = socket.object->GetOwn(kSocketIdSlot);
  return slot != nullptr && slot->IsNumber() ? static_cast<std::uint64_t>(slot->number) : 0u;
}

}  // namespace

js::Value DomBindings::LiveSockets() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kSocketsKey);
      existing != nullptr && existing->IsObject()) {
    return *existing;
  }
  const Value list = interpreter_->NewArrayValue({});
  if (list.IsObject()) {
    interfaces_.object->Set(kSocketsKey, list);
  }
  return list;
}

js::Value DomBindings::SocketWithId(std::uint64_t id) {
  const Value list = LiveSockets();
  if (!list.IsObject()) {
    return Value::Undefined();
  }
  for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
    const Value socket = list.object->GetElement(i);
    if (IdOf(socket) == id) {
      return socket;
    }
  }
  return Value::Undefined();
}

void DomBindings::ForgetSocket(std::uint64_t id) {
  const Value list = LiveSockets();
  if (!list.IsObject()) {
    return;
  }
  // Rebuilt without it rather than spliced: an array with a hole in it is a
  // `readyState` scan that reads `undefined`, and the list is at most a handful of
  // entries.
  std::vector<Value> kept;
  for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
    const Value socket = list.object->GetElement(i);
    if (IdOf(socket) != id) {
      kept.push_back(socket);
    }
  }
  const Value replacement = interpreter_->NewArrayValue(std::move(kept));
  if (replacement.IsObject() && interfaces_.IsObject()) {
    interfaces_.object->Set(kSocketsKey, replacement);
  }
}

void DomBindings::InstallWebSocket() {
  if (sockets_ == nullptr) {
    // No socket source behind this layer, so no `WebSocket` at all -- ADR 0012's rule,
    // and the sharp case for it: a page that finds `WebSocket` and gets a constructor
    // that never fires `open` waits forever, where a page that finds nothing falls back
    // to polling and works.
    return;
  }

  const Value constructor =
      interpreter_->NewNativeValue("WebSocket", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->sockets_ == nullptr) {
          return Value::Undefined();
        }
        std::string url;
        if (!CoerceToString(call, Argument(call.arguments, 0), url)) {
          return call.ThrownValue();
        }
        const Value socket = call.interpreter.NewObjectValue();
        if (!socket.IsObject()) {
          return socket;
        }
        // The id first: a refusal is id 0, and then the object exists with
        // `readyState` CLOSED rather than not existing. That is what the specification
        // says a `WebSocket` to a URL the browser refuses looks like from script -- the
        // constructor does not throw for a policy refusal, it produces a socket that
        // closes.
        const std::uint64_t id = owner->sockets_->OpenSocket(url);
        socket.object->Set(kSocketIdSlot, Value::Number(static_cast<double>(id)));
        socket.object->Set(kOwnerSlot, PointerValue(owner));
        socket.object->Set("url", Value::String(url));
        socket.object->Set("readyState", Value::Number(id == 0 ? kClosed : kConnecting));
        socket.object->Set("bufferedAmount", Value::Number(0));
        // `binaryType` exists and answers, because a page reads it before it decides how
        // to treat a message. Only `"blob"` is refused-by-absence: there is no `Blob`
        // here, so a message arrives as a string either way and saying otherwise would
        // be the stub ADR 0012 forbids.
        socket.object->Set("binaryType", Value::String("arraybuffer"));

        const auto method = [&call, owner, id](const char* name) {
          const Value function =
              call.interpreter.NewNativeValue(name, [](NativeCall& inner) -> Value {
                DomBindings* self = OwnerOf(inner);
                const Value* which =
                    inner.callee == nullptr ? nullptr : inner.callee->GetOwn("#socket-method");
                const Value* id_slot =
                    inner.callee == nullptr ? nullptr : inner.callee->GetOwn(kSocketIdSlot);
                if (self == nullptr || self->sockets_ == nullptr || which == nullptr ||
                    id_slot == nullptr) {
                  return Value::Undefined();
                }
                const std::uint64_t socket_id = static_cast<std::uint64_t>(id_slot->number);
                const std::string what = js::ToString(*which);
                if (what == "send") {
                  const std::string data = js::ToString(Argument(inner.arguments, 0));
                  if (!self->sockets_->SendSocket(socket_id, data, true)) {
                    // The specified failure for a send on a socket that is not open, and
                    // a page catches it: sending into a socket that silently dropped the
                    // message is a page that believes it delivered.
                    return inner.Throw("InvalidStateError",
                                       "WebSocket is not open");
                  }
                  return Value::Undefined();
                }
                // `close([code[, reason]])`. 1000 is "normal closure", which is what a
                // page means when it passes nothing.
                const Value code = Argument(inner.arguments, 0);
                const std::uint16_t status =
                    code.IsUndefined() ? 1000u
                                       : static_cast<std::uint16_t>(js::ToNumber(code));
                self->sockets_->CloseSocket(socket_id, status,
                                            js::ToString(Argument(inner.arguments, 1)));
                if (const Value target = self->SocketWithId(socket_id); target.IsObject()) {
                  target.object->Set("readyState", Value::Number(kClosing));
                }
                return Value::Undefined();
              });
          if (function.IsObject()) {
            function.object->Set(kOwnerSlot, PointerValue(owner));
            function.object->Set(kSocketIdSlot, Value::Number(static_cast<double>(id)));
            function.object->Set("#socket-method", Value::String(name));
          }
          return function;
        };
        socket.object->Set("send", method("send"));
        socket.object->Set("close", method("close"));
        // An event target in the small way this browser needs: `addEventListener` is the
        // window's mechanism and a socket is not the window, so the four `on*` handlers
        // are the whole interface. A page that uses `addEventListener('message')` on a
        // socket finds nothing rather than a listener that never fires -- which is the
        // next thing to build here, and is written down rather than stubbed.
        socket.object->Set("onopen", Value::Null());
        socket.object->Set("onmessage", Value::Null());
        socket.object->Set("onclose", Value::Null());
        socket.object->Set("onerror", Value::Null());

        if (id != 0) {
          const Value list = owner->LiveSockets();
          if (list.IsObject()) {
            std::vector<Value> kept;
            for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
              kept.push_back(list.object->GetElement(i));
            }
            kept.push_back(socket);
            const Value replacement = call.interpreter.NewArrayValue(std::move(kept));
            if (replacement.IsObject() && owner->interfaces_.IsObject()) {
              owner->interfaces_.object->Set(kSocketsKey, replacement);
            }
          }
        }
        return socket;
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, PointerValue(this));
  // The four `readyState` constants, which real code compares against by name.
  constructor.object->Set("CONNECTING", Value::Number(kConnecting));
  constructor.object->Set("OPEN", Value::Number(kOpen));
  constructor.object->Set("CLOSING", Value::Number(kClosing));
  constructor.object->Set("CLOSED", Value::Number(kClosed));
  interpreter_->Global()->Set("WebSocket", constructor);
  interpreter_->GlobalScope()->Declare("WebSocket", constructor, false);
}

bool DomBindings::DeliverSocketOpen(std::uint64_t id) {
  const Value socket = SocketWithId(id);
  if (!socket.IsObject()) {
    return false;
  }
  socket.object->Set("readyState", Value::Number(kOpen));
  const Value* handler = socket.object->GetOwn("onopen");
  if (handler == nullptr || !handler->IsObject()) {
    return false;
  }
  const Value event = interpreter_->NewObjectValue();
  if (event.IsObject()) {
    event.object->Set("type", Value::String("open"));
    event.object->Set("target", socket);
  }
  interpreter_->CallFunction(*handler, socket, {event});
  return true;
}

bool DomBindings::DeliverSocketMessage(std::uint64_t id, const std::string& data, bool text) {
  const Value socket = SocketWithId(id);
  if (!socket.IsObject()) {
    return false;
  }
  const Value* handler = socket.object->GetOwn("onmessage");
  if (handler == nullptr || !handler->IsObject()) {
    return false;
  }
  const Value event = interpreter_->NewObjectValue();
  if (event.IsObject()) {
    event.object->Set("type", Value::String("message"));
    event.object->Set("target", socket);
    // A string either way, and `binaryType` says `arraybuffer` -- which is a known
    // deviation rather than a lie about the bytes: there is no `ArrayBuffer` wrapper on
    // this seam yet, and the data is intact. A page that treats a binary message as text
    // gets its bytes; one that calls `.byteLength` on it gets undefined, which is the
    // part left to build.
    event.object->Set("data", Value::String(data));
    event.object->Set("isText", Value::Bool(text));
  }
  interpreter_->CallFunction(*handler, socket, {event});
  return true;
}

bool DomBindings::DeliverSocketClose(std::uint64_t id, std::uint16_t code,
                                     const std::string& reason, bool clean, bool failed) {
  const Value socket = SocketWithId(id);
  if (!socket.IsObject()) {
    return false;
  }
  socket.object->Set("readyState", Value::Number(kClosed));
  bool ran = false;
  // `error` before `close`, which is the order the specification gives and the order a
  // page depends on: a handler that retries on error must run before the one that tears
  // the connection's state down.
  if (failed) {
    if (const Value* on_error = socket.object->GetOwn("onerror");
        on_error != nullptr && on_error->IsObject()) {
      const Value event = interpreter_->NewObjectValue();
      if (event.IsObject()) {
        event.object->Set("type", Value::String("error"));
        event.object->Set("target", socket);
      }
      interpreter_->CallFunction(*on_error, socket, {event});
      ran = true;
    }
  }
  if (const Value* on_close = socket.object->GetOwn("onclose");
      on_close != nullptr && on_close->IsObject()) {
    const Value event = interpreter_->NewObjectValue();
    if (event.IsObject()) {
      event.object->Set("type", Value::String("close"));
      event.object->Set("target", socket);
      event.object->Set("code", Value::Number(code));
      event.object->Set("reason", Value::String(reason));
      // `wasClean` is what a page keys a reconnect off, and it is a different question
      // from the code: a dropped connection has no code and was not clean.
      event.object->Set("wasClean", Value::Bool(clean));
    }
    interpreter_->CallFunction(*on_close, socket, {event});
    ran = true;
  }
  ForgetSocket(id);
  return ran;
}


// --- EventSource ----------------------------------------------------------------------
//
// The same table and the same absence rule as `WebSocket`. What differs is that a stream
// *reconnects* on its own, so `readyState` goes back to CONNECTING on a drop rather than to
// CLOSED -- and a page reads exactly that to show "reconnecting".
void DomBindings::InstallEventSource() {
  if (sockets_ == nullptr) {
    return;
  }
  const Value constructor =
      interpreter_->NewNativeValue("EventSource", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->sockets_ == nullptr) {
          return Value::Undefined();
        }
        std::string url;
        if (!CoerceToString(call, Argument(call.arguments, 0), url)) {
          return call.ThrownValue();
        }
        const Value stream = call.interpreter.NewObjectValue();
        if (!stream.IsObject()) {
          return stream;
        }
        const std::uint64_t id = owner->sockets_->OpenEventSource(url);
        stream.object->Set(kSocketIdSlot, Value::Number(static_cast<double>(id)));
        stream.object->Set(kOwnerSlot, PointerValue(owner));
        stream.object->Set("url", Value::String(url));
        stream.object->Set("readyState", Value::Number(id == 0 ? kClosed : kConnecting));
        stream.object->Set("onopen", Value::Null());
        stream.object->Set("onmessage", Value::Null());
        stream.object->Set("onerror", Value::Null());
        // `withCredentials` is absent rather than false: a page that reads it is asking
        // whether it can send cookies cross-origin, and answering `false` would be a claim
        // about a code path that does not exist here. ADR 0012's rule.
        const Value close = call.interpreter.NewNativeValue("close", [](NativeCall& inner) -> Value {
          DomBindings* self = OwnerOf(inner);
          const Value* id_slot =
              inner.callee == nullptr ? nullptr : inner.callee->GetOwn(kSocketIdSlot);
          if (self == nullptr || self->sockets_ == nullptr || id_slot == nullptr) {
            return Value::Undefined();
          }
          const std::uint64_t stream_id = static_cast<std::uint64_t>(id_slot->number);
          if (const Value target = self->SocketWithId(stream_id); target.IsObject()) {
            target.object->Set("readyState", Value::Number(kClosed));
          }
          self->sockets_->CloseEventSource(stream_id);
          self->ForgetSocket(stream_id);
          return Value::Undefined();
        });
        if (close.IsObject()) {
          close.object->Set(kOwnerSlot, PointerValue(owner));
          close.object->Set(kSocketIdSlot, Value::Number(static_cast<double>(id)));
        }
        stream.object->Set("close", close);

        if (id != 0) {
          const Value list = owner->LiveSockets();
          if (list.IsObject()) {
            std::vector<Value> kept;
            for (std::size_t i = 0; i < list.object->ElementCount(); ++i) {
              kept.push_back(list.object->GetElement(i));
            }
            kept.push_back(stream);
            const Value replacement = call.interpreter.NewArrayValue(std::move(kept));
            if (replacement.IsObject() && owner->interfaces_.IsObject()) {
              owner->interfaces_.object->Set(kSocketsKey, replacement);
            }
          }
        }
        return stream;
      });
  if (!constructor.IsObject()) {
    return;
  }
  constructor.object->Set(kOwnerSlot, PointerValue(this));
  constructor.object->Set("CONNECTING", Value::Number(kConnecting));
  constructor.object->Set("OPEN", Value::Number(kOpen));
  constructor.object->Set("CLOSED", Value::Number(kClosed));
  interpreter_->Global()->Set("EventSource", constructor);
  interpreter_->GlobalScope()->Declare("EventSource", constructor, false);
}

bool DomBindings::DeliverEventSourceOpen(std::uint64_t id) {
  const Value stream = SocketWithId(id);
  if (!stream.IsObject()) {
    return false;
  }
  stream.object->Set("readyState", Value::Number(kOpen));
  const Value* handler = stream.object->GetOwn("onopen");
  if (handler == nullptr || !handler->IsObject()) {
    return false;
  }
  const Value event = interpreter_->NewObjectValue();
  if (event.IsObject()) {
    event.object->Set("type", Value::String("open"));
    event.object->Set("target", stream);
  }
  interpreter_->CallFunction(*handler, stream, {event});
  return true;
}

bool DomBindings::DeliverEventSourceMessage(std::uint64_t id, const std::string& type,
                                            const std::string& data,
                                            const std::string& last_id) {
  const Value stream = SocketWithId(id);
  if (!stream.IsObject()) {
    return false;
  }
  // A named event goes to `on<name>` if the page assigned one, and to `onmessage`
  // otherwise -- which is what `addEventListener('ping')` would do and is as close as this
  // gets without one. A page that uses the listener form finds nothing rather than a
  // listener that never fires, and that is written where the gap is.
  const std::string slot = type.empty() ? std::string("onmessage") : "on" + type;
  const Value* handler = stream.object->GetOwn(slot.c_str());
  if (handler == nullptr || !handler->IsObject()) {
    handler = stream.object->GetOwn("onmessage");
  }
  if (handler == nullptr || !handler->IsObject()) {
    return false;
  }
  const Value event = interpreter_->NewObjectValue();
  if (event.IsObject()) {
    event.object->Set("type", Value::String(type.empty() ? std::string("message") : type));
    event.object->Set("target", stream);
    event.object->Set("data", Value::String(data));
    event.object->Set("lastEventId", Value::String(last_id));
  }
  interpreter_->CallFunction(*handler, stream, {event});
  return true;
}

bool DomBindings::DeliverEventSourceError(std::uint64_t id, bool permanent) {
  const Value stream = SocketWithId(id);
  if (!stream.IsObject()) {
    return false;
  }
  // **CONNECTING on a retryable drop, CLOSED only when it has given up.** A page reads
  // exactly this to tell "reconnecting" from "failed", and getting it backwards makes a
  // page tear down a stream the browser is about to re-open.
  stream.object->Set("readyState", Value::Number(permanent ? kClosed : kConnecting));
  const Value* handler = stream.object->GetOwn("onerror");
  bool ran = false;
  if (handler != nullptr && handler->IsObject()) {
    const Value event = interpreter_->NewObjectValue();
    if (event.IsObject()) {
      event.object->Set("type", Value::String("error"));
      event.object->Set("target", stream);
    }
    interpreter_->CallFunction(*handler, stream, {event});
    ran = true;
  }
  if (permanent) {
    ForgetSocket(id);
  }
  return ran;
}

}  // namespace microbrowser::bindings
