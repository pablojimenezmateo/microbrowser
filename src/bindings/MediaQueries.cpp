// `window.matchMedia`: the same question a stylesheet asks, asked from script.
//
// youtube's kevlar bundle calls it and stops without it. So does every
// component that has a wide layout and a narrow one and picks between them in
// JavaScript rather than in CSS -- which is most of them, because the choice is
// usually "render a different subtree", not "style the same one differently".
//
// **It goes through `bindings::GeometrySource`, not through a media context of
// its own.** The evaluator is in `src/css` and this module may not see it, so
// the dependency is inverted the way ADR 0015 inverted layout -- but that is
// only half the reason. The other half is that `matchMedia('(max-width:700px)')`
// and `window.innerWidth` are two spellings of one question, and a browser that
// answered them through two seams would eventually answer them differently:
// a page's stylesheet and its script disagreeing about which layout it is in is
// a class of bug with no symptom except a broken page.
//
// `matches` is an accessor rather than a stored boolean, so it is never stale.
// The list is also *live* in the other sense: a viewport change fires `change`
// at whatever is listening, from the one place per frame that already samples
// geometry (see DeliverViewObservations). A `matchMedia` whose listeners never
// fired would be the stub ADR 0012 forbids -- a page would attach one, get the
// initial state right, and then be wrong for the rest of its life.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// The query text, and what it evaluated to the last time anything looked.
// Both on the list object: the collector can see a property and cannot see a
// `js::Value` in a C++ field, and the remembered answer is what makes `change`
// fire on a *change* rather than on every frame.
constexpr const char* kQuerySlot = "#mediaQuery";
constexpr const char* kLastMatchSlot = "#mediaMatched";
constexpr const char* kHandlerSlot = "#onchange";
// Every list `matchMedia` handed out, so a viewport change can find them all.
// On the interfaces object, which is already a GC root.
constexpr const char* kMediaListsSlot = "#mediaLists";

}  // namespace

void DomBindings::InstallMatchMedia() {
  if (interpreter_ == nullptr || geometry_ == nullptr) {
    return;
  }
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value list_interface = MakeInterface("MediaQueryList", InterfaceNamed("EventTarget"));
  if (!list_interface.IsObject()) {
    return;
  }

  // `matches`, asked afresh every read. A stored boolean would be a snapshot of
  // whatever the viewport was when the list was made, which is exactly the
  // answer a page reads `matchMedia` to avoid.
  const Value matches = interpreter_->NewNativeValue("matches", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const Value* query =
        call.self.IsObject() ? call.self.object->GetOwn(kQuerySlot) : nullptr;
    if (owner == nullptr || owner->geometry_ == nullptr || query == nullptr) {
      return Value::Bool(false);
    }
    return Value::Bool(owner->geometry_->QueryMediaMatches(js::ToString(*query)));
  });
  if (matches.IsObject()) {
    matches.object->Set(kOwnerSlot, OwnerValue(this));
    list_interface.object->DefineAccessor("matches", matches.object, nullptr);
  }

  const Value get_handler = interpreter_->NewNativeValue("onchange", [](NativeCall& call) {
    const Value* found =
        call.self.IsObject() ? call.self.object->GetOwn(kHandlerSlot) : nullptr;
    return found == nullptr ? Value::Null() : *found;
  });
  const Value set_handler = interpreter_->NewNativeValue("onchange", [](NativeCall& call) {
    if (call.self.IsObject()) {
      call.self.object->SetHidden(kHandlerSlot, Argument(call.arguments, 0));
    }
    return Value::Undefined();
  });
  if (get_handler.IsObject() && set_handler.IsObject()) {
    list_interface.object->DefineAccessor("onchange", get_handler.object, set_handler.object);
  }

  // `addListener`/`removeListener` are the older spelling and are *not*
  // decoration: they predate MediaQueryList being an EventTarget, and a great
  // deal of shipped script -- including polyfills that feature-detect
  // `addEventListener` on the list and fall back -- still calls them. They are
  // aliases rather than a second mechanism, so a page cannot end up with a
  // listener in a registry nothing dispatches to.
  for (const char* pair : {"addListener", "removeListener"}) {
    const std::string forward =
        std::string(pair[0] == 'a' ? "addEventListener" : "removeEventListener");
    const Value alias = interpreter_->NewNativeValue(pair, [forward](NativeCall& call) -> Value {
      const Value* target = call.self.IsObject() ? call.self.object->Get(forward) : nullptr;
      if (target == nullptr || !target->IsObject()) {
        return Value::Undefined();
      }
      const js::Result outcome = call.interpreter.CallFunction(
          *target, call.self, {Value::String(std::string("change")), Argument(call.arguments, 0)});
      if (outcome.completion == js::Completion::Throw) {
        return call.ThrowValue(outcome.value);
      }
      return Value::Undefined();
    });
    if (alias.IsObject()) {
      list_interface.object->Set(pair, alias);
    }
  }

  DomBindings* self = this;
  const Value match_media = interpreter_->NewNativeValue(
      "matchMedia", [self, list_interface](NativeCall& call) -> Value {
        const Value list = call.interpreter.NewObjectValue();
        if (!list.IsObject()) {
          return Value::Undefined();
        }
        const std::string query = js::ToString(Argument(call.arguments, 0));
        list.object->SetPrototype(list_interface.object);
        list.object->SetHidden(kQuerySlot, Value::String(query));
        // `media` is the query as written. Serializing it back the way the
        // specification asks would need the parser, which lives on the other
        // side of the seam -- and a page reads this to identify the list it got
        // rather than to compare texts.
        list.object->Set("media", Value::String(query));
        list.object->SetHidden(
            kLastMatchSlot,
            Value::Bool(self->geometry_ != nullptr &&
                        self->geometry_->QueryMediaMatches(query)));
        self->InstallEventMethods(list);
        self->TrackMediaQueryList(list);
        return list;
      });
  if (match_media.IsObject()) {
    match_media.object->Set(kOwnerSlot, OwnerValue(this));
    interpreter_->Global()->Set("matchMedia", match_media);
    interpreter_->GlobalScope()->Declare("matchMedia", match_media, false);
  }
}

// Known-crude, and written here rather than left to be discovered: **the
// tracked list grows for the life of the page.** A real browser holds a
// MediaQueryList weakly, so one a page makes and drops stops costing anything;
// here every `matchMedia` call adds an entry that a viewport change has to
// re-evaluate. The cost is one string compare per list per *changed* viewport,
// not per frame, and a page that calls `matchMedia` in a loop is unusual --
// 5,000 lists is a measured 1.4s page with no visible cost. The fix when it
// matters is a weak set, which this engine has (`WeakRef`) and this module has
// no way to reach yet.
void DomBindings::TrackMediaQueryList(const js::Value& list) {
  if (!interfaces_.IsObject()) {
    return;
  }
  Value tracked;
  if (const Value* found = interfaces_.object->GetOwn(kMediaListsSlot)) {
    tracked = *found;
  } else {
    tracked = interpreter_->NewArrayValue({});
    interfaces_.object->Set(kMediaListsSlot, tracked);
  }
  if (tracked.IsObject()) {
    tracked.object->SetElement(tracked.object->ElementCount(), list);
  }
}

bool DomBindings::DeliverMediaQueryChanges() {
  // Deliberately not creating the list: a page that never called `matchMedia`
  // must not allocate an array per frame for the privilege of having none.
  if (interpreter_ == nullptr || geometry_ == nullptr || !interfaces_.IsObject()) {
    return false;
  }
  const Value* tracked = interfaces_.object->GetOwn(kMediaListsSlot);
  if (tracked == nullptr || !tracked->IsObject() || tracked->object->ElementCount() == 0) {
    return false;
  }
  const Value lists = *tracked;

  // Two passes, for the reason the view observers have two: a handler may
  // change the document, and a list sampled after one ran would be describing a
  // different page than the list before it.
  std::vector<Value> changed;
  const std::size_t count = lists.object->ElementCount();
  for (std::size_t i = 0; i < count; ++i) {
    const Value list = lists.object->GetElement(i);
    if (!list.IsObject()) {
      continue;
    }
    const Value* query = list.object->GetOwn(kQuerySlot);
    const Value* last = list.object->GetOwn(kLastMatchSlot);
    if (query == nullptr || last == nullptr) {
      continue;
    }
    const bool now = geometry_->QueryMediaMatches(js::ToString(*query));
    if (now == js::ToBoolean(*last)) {
      continue;
    }
    list.object->SetHidden(kLastMatchSlot, Value::Bool(now));
    changed.push_back(list);
  }

  bool ran = false;
  for (const Value& list : changed) {
    const Value event = interpreter_->NewObjectValue();
    if (!event.IsObject()) {
      continue;
    }
    event.object->Set("type", Value::String(std::string("change")));
    event.object->Set("target", list);
    event.object->Set("media", list.object->Get("media") == nullptr
                                   ? Value::String(std::string())
                                   : *list.object->Get("media"));
    const Value* now = list.object->GetOwn(kLastMatchSlot);
    event.object->Set("matches", now == nullptr ? Value::Bool(false) : *now);
    if (const Value* handler = list.object->GetOwn(kHandlerSlot);
        handler != nullptr && handler->IsObject() && handler->object->IsCallable()) {
      const js::Result outcome = interpreter_->CallFunction(*handler, list, {event});
      if (outcome.completion == js::Completion::Throw) {
        interpreter_->ReportUncaught(outcome.value, "media query change handler");
      }
      ran = true;
    }
    if (const Value* dispatch = list.object->Get("dispatchEvent");
        dispatch != nullptr && dispatch->IsObject()) {
      const js::Result outcome = interpreter_->CallFunction(*dispatch, list, {event});
      if (outcome.completion == js::Completion::Throw) {
        interpreter_->ReportUncaught(outcome.value, "media query change listener");
      }
      ran = true;
    }
  }
  return ran;
}

}  // namespace microbrowser::bindings
