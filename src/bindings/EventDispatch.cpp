#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "dom/FlatTree.h"

#include <cstddef>
#include <string>
#include <string_view>
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

// The compiled handler behind an `on*` **content attribute**, or undefined.
//
// HTML calls an uncompiled one an *internal raw uncompiled handler* and
// compiles it the first time anything looks: which is why this is asked from
// the dispatch path rather than from the parser or from `setAttribute`. There
// is no hook to add and an element without the attribute pays one lookup it
// was already paying.
//
// **Gated on `allowed`, which is this document's CSP answer.** An event
// handler attribute is the one inline script that only `'unsafe-inline'` can
// permit -- it carries no nonce and CSP never hashes it -- and this module may
// not see `src/csp`, so the answer arrives as a flag (ADR 0008). The default
// is *deny*: a path from markup to running code that is on until somebody
// remembers to turn it off is the wrong default.
//
// Cached against the attribute's own text, so changing the attribute
// recompiles and removing it stops answering. Caching against the type alone
// would make `el.setAttribute('onclick', …)` a one-time write.
//
// **What is deliberately not here, so nobody assumes it is:** the *IDL*
// attribute. HTML makes `el.onclick` and the content attribute one slot, so
// reading `el.onclick` on an element that only has the markup should compile
// it and hand it back. Here it still answers undefined -- the attribute is
// reachable only from dispatch. That is a real gap and it is the half that
// wants the reflected-attribute table (task C10), not this one.
js::Value CompiledAttributeHandler(js::Interpreter& interpreter, const js::Value& holder,
                                   const std::string& type, bool allowed) {
  if (!allowed || !holder.IsObject()) {
    return Value::Undefined();
  }
  dom::Node* node = NodeOf(holder);
  if (node == nullptr || !node->IsElement()) {
    return Value::Undefined();
  }
  const std::string* source =
      static_cast<dom::Element&>(*node).GetAttribute("on" + type);
  if (source == nullptr) {
    return Value::Undefined();
  }
  const std::string cache_slot = "#onattr:" + type;
  const std::string source_slot = "#onattrsrc:" + type;
  if (const Value* cached_source = holder.object->GetOwn(source_slot);
      cached_source != nullptr && cached_source->IsString() &&
      cached_source->AsString() == *source) {
    const Value* cached = holder.object->GetOwn(cache_slot);
    return cached == nullptr ? Value::Undefined() : *cached;
  }
  // **A bound, and it is not tidiness.** `Interpreter::Run` begins a host turn,
  // which resets the step budget, and it retains the parsed program for the
  // life of the page -- both correct for a `<script>`, which a document has a
  // handful of. A handler compiled from a *dispatch* is neither: a page can
  // write `el.setAttribute('onclick', i++)` in a loop and dispatch, and every
  // iteration would compile a new text, refresh the budget the loop is being
  // metered against, and add an AST. That is a hang a page can drive, which is
  // the one thing this browser's other bounds exist to stop. Counted on the
  // global for the reason the live-range registry is: it is per document, and
  // a C++ field the collector cannot see is a field this module has got wrong
  // before.
  constexpr const char* kCompileCountSlot = "#onattrCompiles";
  constexpr double kMaxHandlerCompiles = 10000;
  js::Object* global = interpreter.Global();
  double compiles = 0;
  if (global != nullptr) {
    if (const Value* counted = global->GetOwn(kCompileCountSlot); counted != nullptr) {
      compiles = js::ToNumber(*counted);
    }
    if (compiles >= kMaxHandlerCompiles) {
      // Refused rather than truncated, and the *cache* is still written, so a
      // page past the bound pays one lookup per dispatch rather than a compile
      // it will not get.
      holder.object->SetHidden(source_slot, Value::String(*source));
      holder.object->SetHidden(cache_slot, Value::Undefined());
      return Value::Undefined();
    }
    global->SetHidden(kCompileCountSlot, Value::Number(compiles + 1));
  }
  // One `event` parameter, which is the name the body may use. The element,
  // its form owner and the document are **not** in scope -- HTML puts them
  // there and this does not, so `onclick="remove()"` reaches the global
  // `remove` rather than the element's. That is the same answer
  // `Element.prototype[Symbol.unscopables]` gives for the six mixin methods,
  // and the wrong answer for everything else on the element.
  const js::Result compiled =
      interpreter.Run("(function (event) {\n" + *source + "\n})");
  Value handler = Value::Undefined();
  if (compiled.completion == js::Completion::Throw) {
    // A handler that does not parse is *null*, not a throw at the dispatch
    // that found it: the page's mistake must not stop the event reaching
    // everything else on the path. Reported, because a silent one is a handler
    // that never runs for no visible reason.
    interpreter.ReportUncaught(compiled.value, "event handler attribute");
  } else if (compiled.value.IsObject() && compiled.value.object->IsCallable()) {
    handler = compiled.value;
  }
  holder.object->SetHidden(source_slot, Value::String(*source));
  holder.object->SetHidden(cache_slot, handler);
  return handler;
}


bool DomBindings::RunListenersOn(const js::Value& holder, const js::Value& event,
                                 const std::string& slot, EventPhase phase, EventPhase pass) {
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
  // It is a non-capture listener, so only the bubbling *pass* sees it. An
  // `onclick` that fired on the way down would run before the handlers of every
  // element it contains -- and the test is `pass` rather than `phase`, because
  // the target is on both passes reporting AT_TARGET on each, so keying off the
  // phase would run a page's `onclick` twice for every click on the element
  // that carries it.
  const Value* attribute =
      (type == nullptr || pass == EventPhase::Capturing)
          ? nullptr
          : holder.object->Get("on" + js::ToString(*type));
  // Nothing assigned the property, so the *content attribute* is asked --
  // HTML's raw uncompiled handler, compiled here because this is the first
  // moment anything looks at it. A page that assigned `el.onclick = fn` wins:
  // the property is the handler and the attribute is only its uncompiled
  // form, so the order of these two is the specification's.
  Value compiled_attribute;
  if (type != nullptr && pass != EventPhase::Capturing &&
      (attribute == nullptr || !attribute->IsObject() || !attribute->object->IsCallable())) {
    compiled_attribute = CompiledAttributeHandler(*interpreter_, holder, js::ToString(*type),
                                                  inline_handlers_allowed_);
    if (compiled_attribute.IsObject()) {
      attribute = &compiled_attribute;
    }
  }
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
    const Value* stopped_here = event.object->GetOwn("#stopPropagation");
    return stopped_here != nullptr && js::ToBoolean(*stopped_here);
  }

  // A copy, because a handler is allowed to add or remove listeners and the
  // set that runs is the set that existed when the event reached this node.
  std::vector<Value> handlers;
  for (std::size_t i = 0; i < listeners->object->ElementCount(); ++i) {
    handlers.push_back(listeners->object->GetElement(i));
  }
  for (const Value& entry : handlers) {
    // Which listeners this pass runs, decided by `pass` -- the *direction* --
    // and never by `phase`, which is only what the page reads back.
    //
    // The two are different at the target and that is the whole reason the
    // parameter exists. The target is on both passes, reporting AT_TARGET on
    // each, and each still keeps only its own kind: capture listeners on the
    // way down, non-capture on the way up. Filtering by `phase` instead ran
    // both kinds at the target in registration order, which no ordering of
    // registrations can make match the DOM's own case -- capture,
    // non-capture, capture must run 1, 3, 2.
    //
    // `AtTarget` *as a pass* means "both", which is what a lone target with no
    // tree around it gets: an AbortSignal, an XHR, a slot.
    if (pass != EventPhase::AtTarget &&
        ListenerFlag(entry, kListenerCaptureSlot) != (pass == EventPhase::Capturing)) {
      continue;
    }
    // Removed since the copy was taken, by an earlier listener or by an
    // AbortSignal one of them aborted. The DOM's "removed" flag: the copy
    // fixes *which* listeners this node considers, not that every one of them
    // still exists. Without this, `controller.abort()` from inside a handler
    // still ran every later listener the same controller was meant to cancel.
    if (!ListenerStillRegistered(*listeners, entry)) {
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
  const Value* stopped = event.object->GetOwn("#stopPropagation");
  return stopped != nullptr && js::ToBoolean(*stopped);
}

bool DomBindings::DispatchEventTo(dom::Node& target, const js::Value& event) {
  if (interpreter_ == nullptr || !event.IsObject()) {
    return false;
  }
  // Root across the listener drain: handlers queue microtasks, DrainMicrotasks
  // may Collect, and `event` is only a C++ local. Without this, reading
  // `defaultPrevented` afterwards is a use-after-free (ASAN on youtube.com
  // script `load` → GetOwnProperty on a freed event; Release saw SIGFPE in the
  // same hashtable). Same shape as ValueRoot on thrown completions.
  const js::Interpreter::ValueRoot rooted_event(*interpreter_, event);
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
  //
  // **The target is on both passes**, and that is not a rounding error in the
  // model. A listener registered on the target with `capture: true` runs in the
  // *capturing* pass and one registered with `capture: false` runs in the
  // bubbling pass, both reporting `eventPhase === AT_TARGET`. Before this the
  // target was visited once, in a pass that kept only non-capture listeners, so
  // `el.addEventListener('x', f, true)` followed by `el.dispatchEvent(...)`
  // never called `f` at all -- and the DOM's own ordering test
  // (`[1, 3, 2]` for capture, non-capture, capture) cannot be satisfied by any
  // single pass, whichever way it filters.
  //
  // The bubbling pass runs at the target even for an event that does not
  // bubble: `bubbles` decides whether propagation continues *past* the target,
  // not whether the target's own listeners run.
  bool stopped = false;
  for (std::size_t i = path.size(); i-- > 0 && !stopped;) {
    stopped = RunListenersOn(path[i], event, slot,
                             i == 0 ? EventPhase::AtTarget : EventPhase::Capturing,
                             EventPhase::Capturing);
  }
  if (!stopped) {
    stopped = RunListenersOn(path.front(), event, slot, EventPhase::AtTarget,
                             EventPhase::Bubbling);
  }
  if (!stopped && propagates) {
    for (std::size_t i = 1; i < path.size() && !stopped; ++i) {
      stopped = RunListenersOn(path[i], event, slot, EventPhase::Bubbling,
                               EventPhase::Bubbling);
    }
  }
  // Dispatch is over: `currentTarget` is null, the phase is NONE and the
  // propagation path is empty outside it, which is what a handler that stashed
  // the event and reads it later sees.
  event.object->Set("currentTarget", Value::Null());
  event.object->Set("eventPhase", Value::Number(static_cast<double>(EventPhase::None)));
  InstallComposedPath(event, {});
  // Handlers run as a turn of their own, so anything they queued settles
  // before the event is over -- the same rule a script gets.
  interpreter_->DrainMicrotasks();

  const Value* prevented = event.object->GetOwn("defaultPrevented");
  return prevented != nullptr && js::ToBoolean(*prevented);
}

namespace {

void PopulatePointerMouseFields(const Value& event, const PointerInput& pointer) {
  if (!event.IsObject()) {
    return;
  }
  event.object->Set("clientX", Value::Number(static_cast<double>(pointer.client_x)));
  event.object->Set("clientY", Value::Number(static_cast<double>(pointer.client_y)));
  event.object->Set("pageX", Value::Number(static_cast<double>(pointer.page_x)));
  event.object->Set("pageY", Value::Number(static_cast<double>(pointer.page_y)));
  event.object->Set("screenX", Value::Number(static_cast<double>(pointer.client_x)));
  event.object->Set("screenY", Value::Number(static_cast<double>(pointer.client_y)));
  event.object->Set("button", Value::Number(pointer.button));
  event.object->Set("buttons", Value::Number(pointer.buttons));
  event.object->Set("ctrlKey", Value::Bool(pointer.control));
  event.object->Set("shiftKey", Value::Bool(pointer.shift));
  event.object->Set("altKey", Value::Bool(pointer.alt));
  event.object->Set("metaKey", Value::Bool(pointer.meta));
}

bool IsPointerEventType(std::string_view type) {
  return type == "pointerdown" || type == "pointerup" || type == "pointermove" ||
         type == "pointercancel";
}

}  // namespace

bool DomBindings::DispatchPointerMouse(dom::Element& target, std::string_view type,
                                       const PointerInput& pointer) {
  if (interpreter_ == nullptr) {
    return false;
  }
  // Each trusted pointer/mouse event is an input task: always a fresh step
  // count (even under live frames) and a raised hang-guard ceiling. TD-0039
  // covered the spent-turn case with BeginTask; TD-0045 covers youtube's
  // search-thumb click that burns more than kMaxSteps of Polymer work before
  // preventDefault — without the raised ceiling the SPA never stamps and the
  // engine follows a#thumbnail as a full navigation.
  const js::Interpreter::InputTaskBudget input_budget(*interpreter_);
  const Value event = MakeEvent(std::string(type), true, true, true);
  if (!event.IsObject()) {
    return false;
  }
  const char* prototype_name = IsPointerEventType(type) ? "PointerEvent" : "MouseEvent";
  const Value prototype = EventPrototype(prototype_name, "Event");
  if (prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  PopulatePointerMouseFields(event, pointer);
  if (IsPointerEventType(type)) {
    event.object->Set("pointerId", Value::Number(1.0));
    event.object->Set("pointerType", Value::String("mouse"));
    event.object->Set("isPrimary", Value::Bool(true));
  }
  if (type == "click") {
    event.object->Set("detail", Value::Number(1.0));
  }
  return DispatchEventTo(target, event);
}

bool DomBindings::DispatchClick(dom::Element& target, const PointerInput& pointer) {
  return DispatchPointerMouse(target, "click", pointer);
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

bool DomBindings::DispatchInput(dom::Element& target) {
  if (interpreter_ == nullptr) {
    return false;
  }
  const Value event = MakeEvent("input", true, false, true);
  if (!event.IsObject()) {
    return false;
  }
  const Value prototype = EventPrototype("InputEvent", "UIEvent");
  if (prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  return DispatchEventTo(target, event);
}

bool DomBindings::DispatchKey(dom::Node* target, const KeyInput& key) {
  if (interpreter_ == nullptr) {
    return false;
  }
  dom::Node* node = target != nullptr ? target : static_cast<dom::Node*>(document_);
  if (node == nullptr) {
    return false;
  }
  // Trusted key events share the input-task budget with clicks (TD-0039 /
  // TD-0045): a spent or near-ceiling turn must not starve preventDefault.
  const js::Interpreter::InputTaskBudget input_budget(*interpreter_);
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
  // Stored on the event and read back by `Event.prototype.composedPath`: the
  // path is built before any handler runs -- a handler that reparents the
  // target must not change it -- and it exists **only** while the event is
  // being dispatched. An empty vector is how dispatch says it is over, which
  // is what makes `event.composedPath()` answer `[]` afterwards.
  event.object->SetHidden("#path", interpreter_->NewArrayValue(path));
}

bool DomBindings::DispatchAtWindow(const char* type) {
  const Value event = MakeEvent(type, false, false, true);
  return event.IsObject() && DispatchAtWindowWith(type, event);
}

bool DomBindings::DispatchAtWindowWith(const char* type, const js::Value& event) {
  if (interpreter_ == nullptr || !event.IsObject()) {
    return false;
  }
  // Same root as DispatchEventTo: the drain below can Collect.
  const js::Interpreter::ValueRoot rooted_event(*interpreter_, event);
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
  const std::uint32_t saved_depth = trusted_script_depth_;
  if (csp_script_strict_dynamic_) {
    PushTrustedScriptContext();
  }
  DispatchEventTo(*document_, event);
  trusted_script_depth_ = saved_depth;
  return true;
}

bool DomBindings::NotifyLoad() {
  if (interpreter_ == nullptr) {
    return false;
  }
  SetReadyState("complete");
  // At the window, not at the document: `load` does not bubble, and
  // `window.onload` is where every page listens for it.
  const std::uint32_t saved_depth = trusted_script_depth_;
  if (csp_script_strict_dynamic_) {
    PushTrustedScriptContext();
  }
  const bool heard = DispatchAtWindow("load");
  trusted_script_depth_ = saved_depth;
  if (heard) {
    util::AddPerformanceCounter(util::PerfCounterId::EngineLoadEvents);
  }
  return heard;
}

bool DomBindings::NotifyWindowResize() {
  return DispatchAtWindow("resize");
}

void DomBindings::NotifyScriptElementEvent(const dom::Element& element, const char* type) {
  if (interpreter_ == nullptr || document_ == nullptr) {
    return;
  }
  const Value event = MakeEvent(type, false, false, false);
  if (!event.IsObject()) {
    return;
  }
  // Root for the outer drain too: DispatchEventTo roots while it runs, then
  // pops; a second DrainMicrotasks here must not free `event` under us either
  // (even though we no longer read it — keep the lifetime honest).
  const js::Interpreter::ValueRoot rooted_event(*interpreter_, event);
  const std::uint32_t saved_depth = trusted_script_depth_;
  PushTrustedScriptContext();
  DispatchEventTo(const_cast<dom::Element&>(element), event);
  interpreter_->DrainMicrotasks();
  trusted_script_depth_ = saved_depth;
}

void DomBindings::DeliverWindowMessage(const js::Value& data) {
  if (interpreter_ == nullptr) {
    return;
  }
  const Value event = interpreter_->NewObjectValue();
  if (!event.IsObject()) {
    return;
  }
  if (const Value prototype = InterfaceNamed("MessageEvent"); prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  event.object->Set("type", Value::String(std::string("message")));
  event.object->Set("data", data);
  event.object->Set("origin", Value::String(std::string()));
  event.object->Set("source", Value::Null());
  const Value window = Value::Obj(interpreter_->Global());
  event.object->Set("target", window);
  DispatchAtWindowWith("message", event);
}

void DomBindings::MaybeCompleteEsmsFeatureDetection() {
  if (interpreter_ == nullptr) {
    return;
  }
  const Value window = Value::Obj(interpreter_->Global());
  const bool listening =
      window.object->GetOwn("#on:message") != nullptr || window.object->Get("onmessage") != nullptr;
  if (!listening) {
    return;
  }
  std::vector<Value> flags;
  flags.push_back(Value::String("esms"));
  flags.push_back(Value::Bool(false));
  flags.push_back(Value::Bool(true));
  for (int i = 0; i < 4; ++i) {
    flags.push_back(Value::Bool(false));
  }
  DeliverWindowMessage(interpreter_->NewArrayValue(std::move(flags)));
}

}  // namespace microbrowser::bindings
