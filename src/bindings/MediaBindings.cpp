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
#include "util/PerformanceCounters.h"

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
        Value error = MakeDomException(call.interpreter, error_name, message);
        call.interpreter.SettleAsyncResult(promise.object, error, true);
        return promise;
      }
      if (what == "pause") {
        owner->media_->Pause(element);
        return Value::Undefined();
      }
      if (what == "load") {
        owner->media_->Load(element);
        return Value::Undefined();
      }
      // `canPlayType`. Same allowlist `MediaSource.isTypeSupported` uses
      // (AddSourceBuffer with a zero source id), because two answers to "can we
      // play this" is the bug CodecId.h forbids. "probably" once a type is on
      // that list -- the decoder process exists (ADR 0031); "" otherwise.
      if (what == "canPlayType") {
        const std::string type = js::ToString(Argument(call.arguments, 0));
        MediaController::AddBufferError error = MediaController::AddBufferError::None;
        owner->media_->AddSourceBuffer(0, type, error);
        if (error == MediaController::AddBufferError::NotSupported) {
          return Value::String(std::string());
        }
        return Value::String(std::string("probably"));
      }
      return Value::String(std::string());
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
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
      if (what == "videoWidth") {
        return Value::Number(static_cast<double>(owner->media_->VideoWidth(element)));
      }
      if (what == "videoHeight") {
        return Value::Number(static_cast<double>(owner->media_->VideoHeight(element)));
      }
      // Absolute URL of the chosen resource. Missing this, youtube's player sees
      // an empty `currentSrc` while `src` holds `blob:…` and reports
      // `fmt.unplayable` (TD-0020) even with MSE at HAVE_ENOUGH_DATA.
      if (what == "currentSrc") {
        const std::string* src = element.GetAttribute("src");
        return Value::String(src == nullptr ? std::string() : *src);
      }
      // No MediaError object yet when nothing failed: `null`, not `undefined`.
      // A missing `error` made feature tests and player lastError paths diverge.
      if (what == "error") {
        return Value::Null();
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
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      getter.object->Set("#media-prop", Value::String(name));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
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
  accessor("videoWidth");
  accessor("videoHeight");
  accessor("currentSrc");
  accessor("error");

  // `buffered` as a TimeRanges snapshot of whatever MediaSource ranges are
  // attached. Absent rather than always-empty when there is no media controller
  // behind this binding layer.
  const Value buffered = interpreter_->NewNativeValue("buffered", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(call.self);
    if (owner == nullptr || owner->media_ == nullptr || node == nullptr || !node->IsElement()) {
      return Value::Undefined();
    }
    return owner->MakeTimeRanges(
        owner->media_->MediaBuffered(static_cast<const dom::Element&>(*node)));
  });
  if (buffered.IsObject()) {
    buffered.object->Set(kOwnerSlot, OwnerValue(this));
    target.object->DefineAccessor("buffered", buffered.object, nullptr);
  }
}

bool DomBindings::DispatchMediaEvent(dom::Element& element, const std::string& type) {
  if (interpreter_ == nullptr) {
    return false;
  }
  if (type == "error") {
    util::AddPerformanceCounter(util::PerfCounterId::MediaErrorEvents);
  }
  // Trusted, and not cancelable: the only caller is the state machine that saw the transition.
  // A page that could fire `canplay` at its own element could make a player believe data
  // arrived -- which is why this is a C++ entry point and not something script can reach.
  //
  // Not bubbling, which is the specification and is load-bearing: media events on a `<video>`
  // inside a list must not reach the list's click-through handler.
  //
  // Same MediaEventBudget as SourceBuffer updateend: FlushMediaEventsForBuffer runs
  // while appendBuffer's frames are still live (TD-0020).
  const js::Interpreter::MediaEventBudget media_budget(*interpreter_);
  const Value event = MakeEvent(type, false, false, true);
  if (!event.IsObject()) {
    return false;
  }
  DispatchEventTo(element, event);
  return true;
}

}  // namespace microbrowser::bindings
