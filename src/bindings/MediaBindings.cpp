// `HTMLMediaElement`, as a page sees it.
//
// ADR 0028 §1. Everything answered here comes through `bindings::MediaController`, which this
// module declares and `src/engine` implements -- so nothing in this file can name the state
// machine, a ring buffer or a device. What is here is the shape of the API and one hard part:
// `play()` returns a promise.
//
// The promise is the reason this is not a thin property list. `play()` resolves when playback
// starts and **rejects with `NotAllowedError` when autoplay is refused**, and that rejection is
// the mechanism every player on the web uses to decide whether to show a play button. A `play()`
// that returned undefined would make those players silently do nothing.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Media.h"
#include "dom/Node.h"
#include "js/Interpreter.h"
#include "js/Value.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

}  // namespace

void DomBindings::InstallMediaElement(const js::Value& target) {
  if (media_ == nullptr) {
    // No controller behind this layer, so `<video>` has no media API at all -- ADR 0012's rule.
    // A page that finds `play` and gets a promise that never settles has no fallback; one that
    // finds nothing shows its own poster and a link.
    return;
  }
  if (!target.IsObject()) {
    return;
  }

  const auto method = [this, &target](const char* name) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Node* node = NodeOf(call.self);
      const Value* which =
          call.callee == nullptr ? nullptr : call.callee->GetOwn("#media-method");
      if (owner == nullptr || owner->media_ == nullptr || node == nullptr || !node->IsElement() ||
          which == nullptr) {
        return Value::Undefined();
      }
      auto& element = static_cast<dom::Element&>(*node);
      if (!owner->media_->IsMedia(element)) {
        // `document.body.play()` is not a thing. Answering about a non-media element would make
        // a typo look like a player that does nothing.
        return call.Throw("TypeError", "not a media element");
      }
      const std::string what = js::ToString(*which);

      if (what == "play") {
        // The promise, and the whole reason it is one. Resolved when playback starts, rejected
        // with the name the page is written to catch.
        const Value promise = call.interpreter.NewPromiseValue();
        if (!promise.IsObject()) {
          return Value::Undefined();
        }
        const MediaController::PlayResult result = owner->media_->Play(element);
        if (result == MediaController::PlayResult::Started) {
          call.interpreter.SettleAsyncResult(promise.object, Value::Undefined(), false);
          return promise;
        }
        const char* error_name =
            result == MediaController::PlayResult::NotAllowed ? "NotAllowedError"
                                                             : "NotSupportedError";
        const char* message = result == MediaController::PlayResult::NotAllowed
                                  ? "play() requires a user gesture unless the element is muted"
                                  : "no supported source";
        Value error = call.interpreter.MakeError(error_name, message);
        call.interpreter.SettleAsyncResult(promise.object, error, true);
        return promise;
      }
      if (what == "pause") {
        owner->media_->Pause(element);
        return Value::Undefined();
      }
      if (what == "load") {
        // `load()` re-runs the resource selection algorithm. Absent behaviour rather than a
        // no-op that returns: a page calling it expects the element to reset, and pretending
        // would leave it with stale state. Recorded in the ledger; refused here so it is
        // visible.
        return call.Throw("NotSupportedError", "load() is not implemented");
      }
      // `canPlayType`. The honest answer for every type is the empty string -- "cannot play" --
      // until a decoder exists, and that is exactly what the API is for: a page asks and picks
      // another source. `"maybe"` here would be a lie a page acts on.
      return Value::String(std::string());
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      native.object->Set("#media-method", Value::String(name));
      target.object->Set(name, native);
    }
  };
  method("play");
  method("pause");
  method("load");
  method("canPlayType");

  // The properties. Accessors rather than values, because every one of them is a question for
  // the far side: a stored copy would be a number that stopped matching the moment a frame was
  // decoded.
  const auto accessor = [this, &target](const char* name) {
    const Value getter = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Node* node = NodeOf(call.self);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#media-prop");
      if (owner == nullptr || owner->media_ == nullptr || node == nullptr || !node->IsElement() ||
          which == nullptr) {
        return Value::Undefined();
      }
      const auto& element = static_cast<const dom::Element&>(*node);
      const std::string what = js::ToString(*which);
      if (what == "currentTime") {
        return Value::Number(owner->media_->CurrentTime(element));
      }
      if (what == "duration") {
        return Value::Number(owner->media_->Duration(element));
      }
      if (what == "volume") {
        return Value::Number(owner->media_->Volume(element));
      }
      if (what == "readyState") {
        return Value::Number(static_cast<double>(owner->media_->ReadyState(element)));
      }
      if (what == "networkState") {
        return Value::Number(static_cast<double>(owner->media_->NetworkState(element)));
      }
      if (what == "paused") {
        return Value::Bool(owner->media_->Paused(element));
      }
      if (what == "ended") {
        return Value::Bool(owner->media_->Ended(element));
      }
      return Value::Bool(owner->media_->Muted(element));
    });
    const Value setter = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      dom::Node* node = NodeOf(call.self);
      const Value* which = call.callee == nullptr ? nullptr : call.callee->GetOwn("#media-prop");
      if (owner == nullptr || owner->media_ == nullptr || node == nullptr || !node->IsElement() ||
          which == nullptr) {
        return Value::Undefined();
      }
      auto& element = static_cast<dom::Element&>(*node);
      const std::string what = js::ToString(*which);
      const Value assigned = Argument(call.arguments, 0);
      if (what == "currentTime") {
        // Assigning `currentTime` *is* a seek, which is the one place this API does something
        // rather than reporting something.
        owner->media_->Seek(element, js::ToNumber(assigned));
      } else if (what == "muted") {
        owner->media_->SetMuted(element, js::ToBoolean(assigned));
      } else if (what == "volume") {
        owner->media_->SetVolume(element, js::ToNumber(assigned));
      }
      // Everything else is read-only, and assigning to it is *silently ignored* rather than
      // thrown -- which is what a read-only IDL attribute does outside strict mode and what
      // pages that set `duration` by mistake depend on.
      return Value::Undefined();
    });
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      getter.object->Set("#media-prop", Value::String(name));
      setter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set("#media-prop", Value::String(name));
      target.object->DefineAccessor(name, getter.object, setter.object);
    }
  };
  accessor("currentTime");
  accessor("duration");
  accessor("volume");
  accessor("readyState");
  accessor("networkState");
  accessor("paused");
  accessor("ended");
  accessor("muted");
}

bool DomBindings::DispatchMediaEvent(dom::Element& element, const std::string& type) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Trusted, and not cancelable: the only caller is the state machine that saw the transition.
  // A page that could fire `canplay` at its own element could make a player believe data
  // arrived -- which is why this is a C++ entry point and not something script can reach.
  //
  // Not bubbling, which is the specification and is load-bearing: media events on a `<video>`
  // inside a list must not reach the list's click-through handler.
  const Value event = MakeEvent(type, false, false, true);
  if (!event.IsObject()) {
    return false;
  }
  DispatchEventTo(element, event);
  return true;
}

}  // namespace microbrowser::bindings
