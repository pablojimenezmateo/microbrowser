// `Element.animate` and the Animation object a page gets back.
//
// TD-0021: youtube loads `web-animations-next-lite`, which writes `el.style`
// every frame when `Element.prototype.animate` is missing. That path is the
// attr-restyle storm. This file is the native answer: keyframes go through
// `AnimationSource` into `engine::Animations`, the same Apply path CSS
// `@keyframes` uses — never through the style attribute.
//
// Declared only when an AnimationSource is behind this layer (ADR 0012).

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/Waapi.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "css/Timing.h"
#include "dom/Node.h"
#include "js/Heap.h"
#include "js/Interpreter.h"
#include "js/Value.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Object;
using js::Value;

constexpr const char* kWaapiIdSlot = "#waapiId";
constexpr const char* kWaapiFinishedSlot = "#waapiFinished";
constexpr const char* kWaapiRegistrySlot = "#waapiRegistry";

std::string ToCssName(const std::string& name) {
  if (name == "cssFloat") {
    return "float";
  }
  std::string out;
  out.reserve(name.size() + 4);
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      if (i > 0) {
        out.push_back('-');
      }
      out.push_back(static_cast<char>(c - 'A' + 'a'));
      continue;
    }
    out.push_back(c);
  }
  if (out.size() > 7 && out.compare(0, 7, "webkit-") == 0) {
    return out.substr(7);
  }
  if (out.size() > 4 && out.compare(0, 4, "moz-") == 0) {
    return out.substr(4);
  }
  return out;
}

bool IsKeyframeMeta(const std::string& name) {
  return name == "offset" || name == "easing" || name == "composite" || name == "computedOffset";
}

bool IsArrayObject(const Value& value) {
  return value.IsObject() && value.object->TargetKind() == Object::Kind::Array;
}

double ParseTimeMs(const Value& value, double fallback) {
  if (value.IsNumber() && std::isfinite(value.number)) {
    return value.number;
  }
  if (value.IsString()) {
    const std::string text = js::ToString(value);
    if (text.size() >= 2 && text.back() == 's') {
      if (text.size() >= 3 && text[text.size() - 2] == 'm') {
        if (const auto ms = util::ParseDouble(std::string_view(text.data(), text.size() - 2))) {
          return *ms;
        }
      } else if (const auto seconds =
                     util::ParseDouble(std::string_view(text.data(), text.size() - 1))) {
        return *seconds * 1000.0;
      }
    } else if (const auto ms = util::ParseDouble(text)) {
      return *ms;
    }
  }
  return fallback;
}

css::AnimationSpec::Fill ParseFill(const std::string& text) {
  if (text == "forwards") {
    return css::AnimationSpec::Fill::Forwards;
  }
  if (text == "backwards") {
    return css::AnimationSpec::Fill::Backwards;
  }
  if (text == "both") {
    return css::AnimationSpec::Fill::Both;
  }
  return css::AnimationSpec::Fill::None;
}

css::AnimationSpec::Direction ParseDirection(const std::string& text) {
  if (text == "reverse") {
    return css::AnimationSpec::Direction::Reverse;
  }
  if (text == "alternate") {
    return css::AnimationSpec::Direction::Alternate;
  }
  if (text == "alternate-reverse") {
    return css::AnimationSpec::Direction::AlternateReverse;
  }
  return css::AnimationSpec::Direction::Normal;
}

WaapiTiming TimingFromOptions(const Value& options) {
  WaapiTiming timing;
  timing.easing = css::EaseTiming();
  if (options.IsNumber() && std::isfinite(options.number)) {
    timing.duration_ms = options.number;
    return timing;
  }
  if (!options.IsObject()) {
    return timing;
  }
  if (const Value* duration = options.object->Get("duration")) {
    timing.duration_ms = ParseTimeMs(*duration, 0.0);
  }
  if (const Value* delay = options.object->Get("delay")) {
    timing.delay_ms = ParseTimeMs(*delay, 0.0);
  }
  if (const Value* iterations = options.object->Get("iterations")) {
    if (iterations->IsNumber() && iterations->number > 0.0) {
      timing.iterations = iterations->number;
    }
  }
  if (const Value* easing = options.object->Get("easing")) {
    css::TimingFunction parsed;
    if (ParseTimingFunction(js::ToString(*easing), parsed)) {
      timing.easing = parsed;
    }
  }
  if (const Value* fill = options.object->Get("fill")) {
    timing.fill = ParseFill(js::ToString(*fill));
  }
  if (const Value* direction = options.object->Get("direction")) {
    timing.direction = ParseDirection(js::ToString(*direction));
  }
  return timing;
}

void AppendDeclarations(Object& frame, std::vector<std::pair<std::string, std::string>>& out) {
  for (const std::string& key : frame.Keys()) {
    if (IsKeyframeMeta(key)) {
      continue;
    }
    const Value* value = frame.GetOwn(key);
    if (value == nullptr) {
      continue;
    }
    out.emplace_back(ToCssName(key), js::ToString(*value));
  }
}

bool ParseKeyframeList(const Value& input, std::vector<WaapiKeyframe>& out) {
  if (!input.IsObject()) {
    return false;
  }
  if (IsArrayObject(input)) {
    const std::size_t length = input.object->ElementCount();
    if (length == 0) {
      return false;
    }
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      const Value entry = input.object->GetElement(i);
      if (!entry.IsObject()) {
        continue;
      }
      WaapiKeyframe frame;
      if (const Value* offset = entry.object->Get("offset")) {
        if (offset->IsNumber() && std::isfinite(offset->number)) {
          frame.offset = std::clamp(offset->number, 0.0, 1.0);
        } else {
          frame.offset = length == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(length - 1);
        }
      } else {
        frame.offset = length == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(length - 1);
      }
      AppendDeclarations(*entry.object, frame.declarations);
      if (!frame.declarations.empty()) {
        out.push_back(std::move(frame));
      }
    }
    return !out.empty();
  }

  const std::vector<std::string>& keys = input.object->Keys();
  std::size_t max_len = 0;
  for (const std::string& key : keys) {
    if (IsKeyframeMeta(key)) {
      continue;
    }
    const Value* value = input.object->GetOwn(key);
    if (value == nullptr) {
      continue;
    }
    if (IsArrayObject(*value)) {
      max_len = std::max(max_len, value->object->ElementCount());
    } else {
      max_len = std::max(max_len, std::size_t{1});
    }
  }
  if (max_len == 0) {
    return false;
  }
  out.assign(max_len, WaapiKeyframe{});
  for (std::size_t i = 0; i < max_len; ++i) {
    out[i].offset = max_len == 1 ? 1.0 : static_cast<double>(i) / static_cast<double>(max_len - 1);
  }
  if (const Value* offsets = input.object->Get("offset")) {
    if (IsArrayObject(*offsets)) {
      for (std::size_t i = 0; i < max_len && i < offsets->object->ElementCount(); ++i) {
        const Value entry = offsets->object->GetElement(i);
        if (entry.IsNumber() && std::isfinite(entry.number)) {
          out[i].offset = std::clamp(entry.number, 0.0, 1.0);
        }
      }
    }
  }
  for (const std::string& key : keys) {
    if (IsKeyframeMeta(key)) {
      continue;
    }
    const Value* value = input.object->GetOwn(key);
    if (value == nullptr) {
      continue;
    }
    const std::string css_name = ToCssName(key);
    if (IsArrayObject(*value)) {
      for (std::size_t i = 0; i < max_len && i < value->object->ElementCount(); ++i) {
        out[i].declarations.emplace_back(css_name, js::ToString(value->object->GetElement(i)));
      }
    } else {
      for (std::size_t i = 0; i < max_len; ++i) {
        out[i].declarations.emplace_back(css_name, js::ToString(*value));
      }
    }
  }
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const WaapiKeyframe& frame) { return frame.declarations.empty(); }),
            out.end());
  return !out.empty();
}

const char* PlayStateName(WaapiPlayState state) {
  switch (state) {
    case WaapiPlayState::Idle:
      return "idle";
    case WaapiPlayState::Running:
      return "running";
    case WaapiPlayState::Paused:
      return "paused";
    case WaapiPlayState::Finished:
      return "finished";
  }
  return "idle";
}

js::Object* RegistryOf(DomBindings& owner, js::Interpreter& interpreter) {
  const Value document = owner.WrapperFor(&owner.Document());
  if (!document.IsObject()) {
    return nullptr;
  }
  const Value* existing = document.object->GetOwn(kWaapiRegistrySlot);
  if (existing != nullptr && existing->IsObject()) {
    return existing->object;
  }
  const Value registry = interpreter.NewObjectValue();
  if (!registry.IsObject()) {
    return nullptr;
  }
  document.object->Set(kWaapiRegistrySlot, registry);
  return registry.object;
}

std::uint64_t IdOf(const Value& animation) {
  if (!animation.IsObject()) {
    return 0;
  }
  const Value* slot = animation.object->GetOwn(kWaapiIdSlot);
  if (slot == nullptr || !slot->IsNumber() || !(slot->number > 0.0)) {
    return 0;
  }
  return static_cast<std::uint64_t>(slot->number);
}

// One Animation instance: either a live programmatic effect id, or an empty
// finished placeholder (WPT empty keyframes / `new Animation()` with no effect).
Value MakeAnimationObject(DomBindings& owner, js::Interpreter& interpreter, std::uint64_t id,
                          bool already_finished) {
  const Value animation = interpreter.NewObjectValue();
  if (!animation.IsObject()) {
    return Value::Undefined();
  }
  if (js::Value* animation_global = interpreter.GlobalScope()->Lookup("Animation")) {
    if (animation_global->IsObject()) {
      if (const Value* proto = animation_global->object->Get("prototype")) {
        if (proto->IsObject()) {
          animation.object->SetPrototype(proto->object);
        }
      }
    }
  }
  if (id != 0) {
    animation.object->Set(kWaapiIdSlot, Value::Number(static_cast<double>(id)));
  } else {
    animation.object->Set("#waapiEmpty", Value::Bool(true));
  }
  animation.object->Set("#waapiPlaybackRate", Value::Number(1.0));
  const Value finished = interpreter.NewPromiseValue();
  if (finished.IsObject()) {
    animation.object->Set(kWaapiFinishedSlot, finished);
    if (already_finished) {
      interpreter.SettleAsyncResult(finished.object, animation, false);
    }
  }
  if (id != 0) {
    if (js::Object* registry = RegistryOf(owner, interpreter)) {
      registry->Set(std::to_string(id), animation);
    }
  }
  return animation;
}

}  // namespace

void DomBindings::InstallWaapi(const js::Value& element_interface) {
  if (animations_ == nullptr || interpreter_ == nullptr || !element_interface.IsObject()) {
    return;
  }

  const Value animation_proto = interpreter_->NewObjectValue();
  if (!animation_proto.IsObject()) {
    return;
  }

  const auto method = [this, &animation_proto](const char* name) {
    const Value native = interpreter_->NewNativeValue(name, [](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      const Value* which =
          call.callee == nullptr ? nullptr : call.callee->GetOwn("#waapi-method");
      if (owner == nullptr || owner->animations_ == nullptr || which == nullptr ||
          !call.self.IsObject()) {
        return Value::Undefined();
      }
      const std::uint64_t id = IdOf(call.self);
      const std::string what = js::ToString(*which);
      // Empty / construct-only Animations have no engine id. play/pause/cancel
      // are still callable so feature detection and `new Animation()` callers
      // do not throw (youtube SPA → watch constructs Animation in a listener).
      if (id == 0) {
        return Value::Undefined();
      }
      if (what == "pause") {
        owner->animations_->PauseAnimation(id);
        return Value::Undefined();
      }
      if (what == "play") {
        owner->animations_->PlayAnimation(id);
        return Value::Undefined();
      }
      if (what == "cancel") {
        owner->animations_->CancelAnimation(id);
        return Value::Undefined();
      }
      if (what == "finish") {
        // Jump to the end by cancelling the running effect and leaving playState
        // to the finished promise path when the engine reports it. A dedicated
        // FinishAnimation seam is TD/ADR material; for constructibility and the
        // lite polyfill's method probe this is enough.
        owner->animations_->CancelAnimation(id);
        return Value::Undefined();
      }
      if (what == "reverse") {
        // Direction flip wants engine support; expose the method so
        // web-animations-next-lite does not replace window.Animation with a
        // style-writing fallback (TD-0021) after seeing an incomplete native.
        return Value::Undefined();
      }
      return Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      native.object->Set("#waapi-method", Value::String(name));
      animation_proto.object->Set(name, native);
    }
  };
  method("pause");
  method("play");
  method("cancel");
  method("finish");
  method("reverse");

  const Value play_state_get =
      interpreter_->NewNativeValue("get playState", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (call.self.IsObject() && call.self.object->GetOwn("#waapiEmpty") != nullptr) {
          return Value::String(std::string("finished"));
        }
        if (owner == nullptr || owner->animations_ == nullptr) {
          return Value::String(std::string("idle"));
        }
        return Value::String(
            std::string(PlayStateName(owner->animations_->AnimationPlayState(IdOf(call.self)))));
      });
  if (play_state_get.IsObject()) {
    play_state_get.object->Set(kOwnerSlot, OwnerValue(this));
    animation_proto.object->DefineAccessor("playState", play_state_get.object, nullptr);
  }

  const Value finished_get =
      interpreter_->NewNativeValue("get finished", [](NativeCall& call) -> Value {
        if (!call.self.IsObject()) {
          return Value::Undefined();
        }
        const Value* promise = call.self.object->GetOwn(kWaapiFinishedSlot);
        return promise == nullptr ? Value::Undefined() : *promise;
      });
  if (finished_get.IsObject()) {
    animation_proto.object->DefineAccessor("finished", finished_get.object, nullptr);
  }

  const Value current_get =
      interpreter_->NewNativeValue("get currentTime", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->animations_ == nullptr) {
          return Value::Null();
        }
        const std::optional<double> local =
            owner->animations_->AnimationCurrentTimeMs(IdOf(call.self));
        return local.has_value() ? Value::Number(*local) : Value::Null();
      });
  const Value current_set =
      interpreter_->NewNativeValue("set currentTime", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->animations_ == nullptr || call.arguments.empty()) {
          return Value::Undefined();
        }
        if (!call.arguments[0].IsNumber() || !std::isfinite(call.arguments[0].number)) {
          return Value::Undefined();
        }
        owner->animations_->SetAnimationCurrentTimeMs(IdOf(call.self), call.arguments[0].number);
        return Value::Undefined();
      });
  if (current_get.IsObject() && current_set.IsObject()) {
    current_get.object->Set(kOwnerSlot, OwnerValue(this));
    current_set.object->Set(kOwnerSlot, OwnerValue(this));
    animation_proto.object->DefineAccessor("currentTime", current_get.object, current_set.object);
  }

  const Value rate_get =
      interpreter_->NewNativeValue("get playbackRate", [](NativeCall& call) -> Value {
        if (!call.self.IsObject()) {
          return Value::Number(1.0);
        }
        const Value* rate = call.self.object->GetOwn("#waapiPlaybackRate");
        return rate != nullptr && rate->IsNumber() ? *rate : Value::Number(1.0);
      });
  const Value rate_set =
      interpreter_->NewNativeValue("set playbackRate", [](NativeCall& call) -> Value {
        if (!call.self.IsObject() || call.arguments.empty() || !call.arguments[0].IsNumber() ||
            !std::isfinite(call.arguments[0].number)) {
          return Value::Undefined();
        }
        call.self.object->Set("#waapiPlaybackRate", call.arguments[0]);
        return Value::Undefined();
      });
  if (rate_get.IsObject() && rate_set.IsObject()) {
    animation_proto.object->DefineAccessor("playbackRate", rate_get.object, rate_set.object);
  }

  const Value start_get =
      interpreter_->NewNativeValue("get startTime", [](NativeCall& call) -> Value {
        if (!call.self.IsObject()) {
          return Value::Null();
        }
        const Value* start = call.self.object->GetOwn("#waapiStartTime");
        return start == nullptr ? Value::Null() : *start;
      });
  const Value start_set =
      interpreter_->NewNativeValue("set startTime", [](NativeCall& call) -> Value {
        if (!call.self.IsObject() || call.arguments.empty()) {
          return Value::Undefined();
        }
        if (call.arguments[0].IsNull() || call.arguments[0].IsUndefined()) {
          call.self.object->Delete("#waapiStartTime");
          return Value::Undefined();
        }
        if (!call.arguments[0].IsNumber() || !std::isfinite(call.arguments[0].number)) {
          return Value::Undefined();
        }
        call.self.object->Set("#waapiStartTime", call.arguments[0]);
        return Value::Undefined();
      });
  if (start_get.IsObject() && start_set.IsObject()) {
    animation_proto.object->DefineAccessor("startTime", start_get.object, start_set.object);
  }

  // Constructible: youtube's watch player path does `new Animation(effect)` in
  // an event listener. Leaving this as Illegal constructor made SPA search→watch
  // land on ytd-player without `#movie_player` / `<video>`. Effect association
  // (KeyframeEffect) is still approximate — null/undefined effect yields an idle
  // finished Animation, which is what `new Animation()` means in WAAPI.
  const Value animation_ctor =
      interpreter_->NewNativeValue("Animation", [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        if (owner == nullptr || owner->interpreter_ == nullptr) {
          return call.Throw("TypeError", "Failed to construct 'Animation'");
        }
        return MakeAnimationObject(*owner, call.interpreter, 0, true);
      });
  if (animation_ctor.IsObject()) {
    animation_ctor.object->Set(kOwnerSlot, OwnerValue(this));
    animation_ctor.object->Set("prototype", animation_proto);
    animation_proto.object->Set("constructor", animation_ctor);
    interpreter_->Global()->Set("Animation", animation_ctor);
    interpreter_->GlobalScope()->Declare("Animation", animation_ctor, false);
  }

  const Value animate = interpreter_->NewNativeValue("animate", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(call.self);
    if (owner == nullptr || owner->animations_ == nullptr || owner->interpreter_ == nullptr ||
        node == nullptr || !node->IsElement()) {
      return call.Throw("TypeError", "Illegal invocation");
    }

    std::vector<WaapiKeyframe> keyframes;
    bool empty_effect = call.arguments.empty() || call.arguments[0].IsNull() ||
                        call.arguments[0].IsUndefined();
    if (!empty_effect) {
      if (IsArrayObject(call.arguments[0]) && call.arguments[0].object->ElementCount() == 0) {
        empty_effect = true;
      } else if (!ParseKeyframeList(call.arguments[0], keyframes)) {
        return call.Throw("TypeError", "Failed to execute 'animate': keyframes are required");
      } else if (keyframes.empty()) {
        empty_effect = true;
      }
    }
    if (empty_effect) {
      return MakeAnimationObject(*owner, call.interpreter, 0, true);
    }

    const WaapiTiming timing = TimingFromOptions(Argument(call.arguments, 1));
    const std::uint64_t id = owner->animations_->StartAnimation(
        static_cast<dom::Element&>(*node), std::move(keyframes), timing);
    if (id == 0) {
      return call.Throw("TypeError", "Failed to execute 'animate': could not start animation");
    }
    return MakeAnimationObject(*owner, call.interpreter, id, false);
  });
  if (animate.IsObject()) {
    animate.object->Set(kOwnerSlot, OwnerValue(this));
    element_interface.object->Set("animate", animate);
  }
}

bool DomBindings::DeliverFinishedAnimations() {
  if (animations_ == nullptr || interpreter_ == nullptr || document_ == nullptr) {
    return false;
  }
  const std::vector<AnimationSource::FinishedAnimation> finished =
      animations_->TakeFinishedAnimations();
  if (finished.empty()) {
    return false;
  }
  const Value document = WrapperFor(document_);
  js::Object* registry = nullptr;
  if (document.IsObject()) {
    const Value* existing = document.object->GetOwn(kWaapiRegistrySlot);
    if (existing != nullptr && existing->IsObject()) {
      registry = existing->object;
    }
  }
  bool settled = false;
  for (const AnimationSource::FinishedAnimation& notice : finished) {
    if (registry == nullptr) {
      continue;
    }
    const std::string key = std::to_string(notice.id);
    const Value* animation = registry->GetOwn(key);
    if (animation == nullptr || !animation->IsObject()) {
      continue;
    }
    const Value* promise = animation->object->GetOwn(kWaapiFinishedSlot);
    if (promise != nullptr && promise->IsObject()) {
      if (notice.cancelled) {
        Value error = MakeDomException(*interpreter_, "AbortError", "The animation was cancelled");
        interpreter_->SettleAsyncResult(promise->object, error, true);
      } else {
        interpreter_->SettleAsyncResult(promise->object, *animation, false);
      }
      settled = true;
    }
    registry->Delete(key);
  }
  return settled;
}

}  // namespace microbrowser::bindings
