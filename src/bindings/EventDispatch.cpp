#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "dom/FlatTree.h"

#include <cstddef>
#include <string>
#include <vector>

#include "util/PerformanceCounters.h"

// Dispatching an event, and the six things the browser dispatches.
//
// Split from EventBindings.cpp, which registers listeners and builds the event
// objects; this is the other half -- what happens when one is fired. The split
// is where ADR 0017 §2 puts the line: **there is one dispatch algorithm and
// every event goes through it.** A click, a key, a `submit`, a scroll and a
// page's own `dispatchEvent` all arrive at DispatchEventTo, because two events
// with two dispatch paths is how a browser ends up with a `preventDefault` that
// works on links and not on forms.
//
// Everything here except the private RunListenersOn is a C++ entry point with
// no script-facing counterpart. The only thing allowed to say a click happened
// is the thing that saw one: an event a page can forge must not be able to
// cause what a real one causes.

namespace microbrowser::bindings {

namespace {

using js::Value;

// Set on the *event* while a passive listener runs. `preventDefault` reads it
// and does nothing, which is the whole point of the flag: a page promises not
// to cancel so the browser can start scrolling before the handler returns.
constexpr const char* kEventInPassiveSlot = "#inPassive";
constexpr const char* kListenerFunctionSlot = "#fn";
constexpr const char* kListenerCaptureSlot = "#capture";
constexpr const char* kListenerPassiveSlot = "#passive";
constexpr const char* kListenerOnceSlot = "#once";

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

bool DomBindings::RunListenersOn(const js::Value& holder, const js::Value& event,
                                 const std::string& slot, EventPhase phase) {
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
  //
  // It is a bubble-phase listener, so the capture pass does not see it. An
  // `onclick` that fired on the way *down* would run before the handlers of
  // every element it contains.
  const Value* attribute =
      (type == nullptr || phase == EventPhase::Capturing)
          ? nullptr
          : holder.object->Get("on" + js::ToString(*type));
  if ((listeners == nullptr || !listeners->IsObject()) &&
      (attribute == nullptr || !attribute->IsObject() || !attribute->object->IsCallable())) {
    return false;
  }
  event.object->Set("currentTarget", holder);
  event.object->Set("eventPhase", Value::Number(static_cast<double>(phase)));

  if (attribute != nullptr && attribute->IsObject() && attribute->object->IsCallable()) {
    const js::Result answer = interpreter_->CallFunction(*attribute, holder, {event});
    if (answer.completion == js::Completion::Throw) {
      // The same for an `on…` attribute handler, which is the other half of the same rule.
      interpreter_->ReportUncaught(answer.value, "event handler");
    }
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
  // set that runs is the set that existed when the event reached this node.
  std::vector<Value> handlers;
  for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
    handlers.push_back(listeners->object->GetElement(i));
  }
  for (const Value& entry : handlers) {
    // At the target both kinds run, in the order they were registered. That is
    // the specification and it is not a corner: a page that registers a capture
    // listener on the element it clicks expects it to run.
    if (phase != EventPhase::AtTarget &&
        ListenerFlag(entry, kListenerCaptureSlot) != (phase == EventPhase::Capturing)) {
      continue;
    }
    // Removed before it is called, not after: a `once` listener that dispatches
    // the same event again must not see itself still registered.
    if (ListenerFlag(entry, kListenerOnceSlot)) {
      ForgetListener(*listeners, entry);
    }
    const bool passive = ListenerFlag(entry, kListenerPassiveSlot);
    if (passive) {
      event.object->Set(kEventInPassiveSlot, Value::Bool(true));
    }
    // `this` is the object the listener was registered on, which is what a
    // handler written as an ordinary function expects.
    //
    // **The result is looked at.** It used to be discarded, and that made an exception in a listener
    // vanish completely -- no console line, no script error, nothing. The specification says an
    // exception in a listener is *reported* and dispatch continues with the next listener, and the
    // reporting half is not decoration: a whole MSE page in session 28 stopped silently at a
    // `ReferenceError` inside a `sourceopen` handler, and the only symptom was output that stopped
    // mid-way. Continuing is right and staying quiet was not.
    const js::Result outcome = interpreter_->CallFunction(ListenerFunction(entry), holder, {event});
    if (outcome.completion == js::Completion::Throw) {
      interpreter_->ReportUncaught(outcome.value, "event listener");
    }
    if (passive) {
      event.object->Set(kEventInPassiveSlot, Value::Bool(false));
    }
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

  // The propagation path: the target, its ancestors, and the window. Built
  // before any handler runs, because a handler that reparents the target must
  // not change which ancestors see the event -- and because the capture phase
  // walks it backwards, which needs it to exist first.
  //
  // The window is on the path even for an event that does not bubble. That is
  // not a detail: capture goes *down* from the window, so a page that captures
  // clicks at the window sees every click, and one that captures a `submit`
  // sees every submission.
  // The walk crosses a shadow boundary through the *host*, which is what makes an
  // event fired inside a component reach a listener on the page. A shadow root has
  // no parent -- deliberately, ADR 0019 §2 -- so this is the one place the two
  // trees are joined for propagation.
  std::vector<js::Value> path;
  std::vector<dom::Node*> nodes;
  path.push_back(WrapperFor(&target));
  nodes.push_back(&target);
  for (dom::Node* walk = &target; walk != nullptr;) {
    if (walk->Parent() != nullptr) {
      walk = walk->Parent();
    } else if (const dom::Element* host = dom::ShadowHostOf(*walk)) {
      walk = const_cast<dom::Element*>(host);
    } else {
      break;
    }
    path.push_back(WrapperFor(walk));
    nodes.push_back(walk);
  }
  path.push_back(Value::Obj(interpreter_->Global()));

  // **Retargeting**, ADR 0019 §5. `target` is not the node the event fired on
  // when that node is inside a shadow tree: it is the outermost host on the path,
  // so a listener on the page sees the *component* rather than a node it was
  // never given a reference to. Without this, a click on a button inside a
  // component reports a target the page cannot have obtained, which is both a
  // leak of the tree's shape and a `target` a page's own code cannot compare
  // against anything it holds.
  std::size_t retargeted = 0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (dom::ShadowHostOf(*nodes[i]) != nullptr) {
      // Still inside a shadow tree, so the visible target is further out.
      retargeted = i + 1 < nodes.size() ? i + 1 : i;
    }
  }
  event.object->Set("target", path[retargeted]);
  // `composedPath()` answers the *whole* path, shadow trees included, and only
  // for an event that says `composed`. A page inside the component uses it to
  // find the real target that retargeting hid, which is why the two exist
  // together: one is what the page outside sees, the other is what the component
  // can still ask for.
  InstallComposedPath(event, path);

  // Down, then at, then up: the three phases, in the one place every event goes
  // through. ADR 0017 §2 -- two events with two dispatch paths is how a browser
  // ends up with a `preventDefault` that works on links and not on forms.
  bool stopped = false;
  for (std::size_t i = path.size(); i-- > 1 && !stopped;) {
    stopped = RunListenersOn(path[i], event, slot, EventPhase::Capturing);
  }
  if (!stopped) {
    stopped = RunListenersOn(path.front(), event, slot, EventPhase::AtTarget);
  }
  if (!stopped && propagates) {
    for (std::size_t i = 1; i < path.size() && !stopped; ++i) {
      stopped = RunListenersOn(path[i], event, slot, EventPhase::Bubbling);
    }
  }
  // Dispatch is over: `currentTarget` is null and the phase is NONE outside it,
  // which is what a handler that stashed the event and reads it later sees.
  event.object->Set("currentTarget", Value::Null());
  event.object->Set("eventPhase", Value::Number(static_cast<double>(EventPhase::None)));
  // Handlers run as a turn of their own, so anything they queued settles
  // before the event is over -- the same rule a script gets.
  interpreter_->DrainMicrotasks();

  const Value* prevented = event.object->GetOwn("defaultPrevented");
  return prevented != nullptr && js::ToBoolean(*prevented);
}

bool DomBindings::DispatchClick(dom::Element& target, const PointerInput& pointer) {
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
  // Where, and with what held. A handler that positions a menu at the pointer
  // reads `clientX`; one that opens a link in a background tab reads `ctrlKey`.
  // Both were unreachable while the message this comes from carried a position
  // and nothing else.
  event.object->Set("clientX", Value::Number(static_cast<double>(pointer.client_x)));
  event.object->Set("clientY", Value::Number(static_cast<double>(pointer.client_y)));
  event.object->Set("pageX", Value::Number(static_cast<double>(pointer.page_x)));
  event.object->Set("pageY", Value::Number(static_cast<double>(pointer.page_y)));
  // `screenX`/`screenY` are deliberately the client coordinates rather than a
  // real screen position. The engine has no window and no screen, and where the
  // user's window sits on their desktop is a fingerprinting bit (ADR 0029) that
  // no page on the compatibility list needs.
  event.object->Set("screenX", Value::Number(static_cast<double>(pointer.client_x)));
  event.object->Set("screenY", Value::Number(static_cast<double>(pointer.client_y)));
  event.object->Set("button", Value::Number(pointer.button));
  event.object->Set("buttons", Value::Number(pointer.buttons));
  event.object->Set("ctrlKey", Value::Bool(pointer.control));
  event.object->Set("shiftKey", Value::Bool(pointer.shift));
  event.object->Set("altKey", Value::Bool(pointer.alt));
  event.object->Set("metaKey", Value::Bool(pointer.meta));
  event.object->Set("detail", Value::Number(1));
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

bool DomBindings::DispatchKey(dom::Node* target, const KeyInput& key) {
  if (interpreter_ == nullptr) {
    return false;
  }
  dom::Node* node = target != nullptr ? target : static_cast<dom::Node*>(document_);
  if (node == nullptr) {
    return false;
  }
  // Trusted, bubbling and cancelable. Cancelable is the load-bearing one: a
  // `preventDefault` on a keydown is how a page stops the character being
  // inserted, and it can only do that because insertion happens after dispatch
  // rather than on the way to it.
  const Value event = MakeEvent(key.down ? "keydown" : "keyup", true, true, true);
  if (!event.IsObject()) {
    return false;
  }
  const Value prototype = EventPrototype("KeyboardEvent", "Event");
  if (prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  event.object->Set("key", Value::String(key.key));
  event.object->Set("code", Value::String(key.code));
  event.object->Set("repeat", Value::Bool(key.repeat));
  event.object->Set("ctrlKey", Value::Bool(key.control));
  event.object->Set("shiftKey", Value::Bool(key.shift));
  event.object->Set("altKey", Value::Bool(key.alt));
  event.object->Set("metaKey", Value::Bool(key.meta));
  // `keyCode` and `which` are deprecated and every page written before 2015
  // reads them. They are the code unit of a single-character `key`, and the
  // uppercase one, which is what every engine reports and what a page comparing
  // against 13 or 27 is expecting.
  double legacy = 0.0;
  if (key.key == "Enter") {
    legacy = 13.0;
  } else if (key.key == "Escape") {
    legacy = 27.0;
  } else if (key.key == "Backspace") {
    legacy = 8.0;
  } else if (key.key == "Tab") {
    legacy = 9.0;
  } else if (key.key.size() == 1) {
    const char c = key.key[0];
    legacy = static_cast<double>(static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c));
  }
  event.object->Set("keyCode", Value::Number(legacy));
  event.object->Set("which", Value::Number(legacy));
  return DispatchEventTo(*node, event);
}

bool DomBindings::DispatchScroll(dom::Element* target) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Not cancelable, because it is reported after the fact: by the time a page
  // hears about a scroll the pixels have already moved, and a `preventDefault`
  // that unscrolled them is a thing no engine has ever offered.
  //
  // On an element it bubbles to the document; on the viewport it is fired at
  // the document and at the window, which is where the other half of pages
  // listen for it. ADR 0018 §3: this runs at most once per frame however many
  // notches arrived, which is what keeps twelve listeners from running twelve
  // times for one wheel.
  //
  // Whether anything is *listening* is asked first, and that is not a
  // micro-optimisation: the caller relays out when this returns true, so a page
  // with no `scroll` handler would run a full layout on every wheel notch --
  // which is precisely the cost ADR 0018 exists to avoid. `DispatchEventTo`
  // cannot answer it, because what it reports is `preventDefault` and a
  // non-cancelable event has none.
  const auto listening = [this](dom::Node* from) {
    const std::string slot = "#on:scroll";
    for (dom::Node* walk = from; walk != nullptr; walk = walk->Parent()) {
      const Value wrapper = WrapperFor(walk);
      if (wrapper.IsObject() && (wrapper.object->GetOwn(slot) != nullptr ||
                                 wrapper.object->GetOwn("onscroll") != nullptr)) {
        return true;
      }
    }
    js::Object* global = interpreter_->Global();
    return global->GetOwn(slot) != nullptr || global->Get("onscroll") != nullptr;
  };

  dom::Node* from = target != nullptr ? static_cast<dom::Node*>(target)
                                      : static_cast<dom::Node*>(document_);
  if (from == nullptr || !listening(from)) {
    return false;
  }
  const Value event = MakeEvent("scroll", true, false, true);
  if (!event.IsObject()) {
    return false;
  }
  DispatchEventTo(*from, event);
  return true;
}

void DomBindings::InstallComposedPath(const js::Value& event,
                                     const std::vector<js::Value>& path) {
  if (interpreter_ == nullptr || !event.IsObject()) {
    return;
  }
  // Stored on the event and handed back by a method, rather than computed when
  // asked: the path is built before any handler runs -- a handler that reparents
  // the target must not change it -- and `composedPath()` has to answer with that
  // same path afterwards.
  const Value stored = interpreter_->NewArrayValue(path);
  event.object->SetHidden("#path", stored);
  const Value method = interpreter_->NewNativeValue("composedPath", [](js::NativeCall& call) {
    const Value* composed =
        call.self.IsObject() ? call.self.object->GetOwn("composed") : nullptr;
    const Value* stored_path =
        call.self.IsObject() ? call.self.object->GetOwn("#path") : nullptr;
    if (stored_path == nullptr || !stored_path->IsObject()) {
      return call.interpreter.NewArrayValue({});
    }
    if (composed == nullptr || !js::ToBoolean(*composed)) {
      // A non-composed event does not escape its tree, so its path stops at the
      // root it was fired in. Answering with the full path would tell a listener
      // outside about nodes the event never reached.
      std::vector<Value> inside;
      for (std::size_t i = 0; i < stored_path->object->ElementCount(); ++i) {
        inside.push_back(stored_path->object->GetElement(i));
      }
      return call.interpreter.NewArrayValue(std::move(inside));
    }
    return *stored_path;
  });
  if (method.IsObject()) {
    event.object->Set("composedPath", method);
  }
}

bool DomBindings::DispatchAtWindow(const char* type) {
  const Value event = MakeEvent(type, false, false, true);
  return event.IsObject() && DispatchAtWindowWith(type, event);
}

bool DomBindings::DispatchAtWindowWith(const char* type, const js::Value& event) {
  if (interpreter_ == nullptr || !event.IsObject()) {
    return false;
  }
  const Value window = Value::Obj(interpreter_->Global());
  const std::string slot = std::string("#on:") + type;
  const bool listening =
      window.object->GetOwn(slot) != nullptr || window.object->Get(std::string("on") + type) != nullptr;
  if (!listening) {
    // Asked before the listeners run rather than before the event is built,
    // because a caller that put fields on the event -- `popstate`'s `state`, a
    // `hashchange`'s two URLs -- has already built it. A page listening for
    // nothing still costs nothing, which is what keeps `load` from relaying out
    // every document that ever finished loading.
    return false;
  }
  event.object->Set("target", window);
  RunListenersOn(window, event, slot, EventPhase::AtTarget);
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

}  // namespace microbrowser::bindings
