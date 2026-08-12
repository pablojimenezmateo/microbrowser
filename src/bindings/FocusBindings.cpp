#include <string>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "html/Focus.h"
#include "html/FormControl.h"

// Focus, as a page sees it and as the browser moves it.
//
// One algorithm, in one place, reached from both sides. `element.focus()` is
// script asking; a click and Tab are the engine telling; and both arrive at
// MoveFocus, because two ways to change focus is how `document.activeElement`
// ends up disagreeing with where the next keystroke goes. ADR 0017 §4 puts the
// state on the document for the same reason -- there is one copy of it, in
// `dom::Document`, and this module and the engine both read that one.
//
// The events are the part that is easy to get subtly wrong, so they are
// written out: `blur` and `focusout` on the element losing it, then `focus` and
// `focusin` on the element gaining it. `blur`/`focus` do not bubble and
// `focusout`/`focusin` do, which is the entire reason the specification has
// four events rather than two -- a delegating handler on a container can only
// hear the bubbling pair. None of the four is cancelable: by the time a page
// hears about a focus change it has happened, and a `preventDefault` that
// un-focused something is a thing no engine has ever offered.

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

bool DomBindings::MoveFocus(dom::Element* target, bool visible) {
  if (document_ == nullptr) {
    return false;
  }
  // A page may ask to focus an element that cannot hold focus -- a `<div>` with
  // no `tabindex`, a disabled input -- and the specification's answer is that
  // nothing happens. Not an error: `focus()` on the wrong node is a no-op every
  // page relies on, and throwing would break more than it reported.
  if (target != nullptr && !html::IsFocusable(*target)) {
    return false;
  }
  dom::Element* previous = document_->Focus().element;
  if (previous == target) {
    // Still record the heuristic: Tab landing back on the element a click
    // focused must show a focus ring, and a click on the element Tab reached
    // must take it away.
    document_->SetFocus(target, visible);
    return false;
  }
  document_->SetFocus(target, visible);

  // The state moves before any handler runs. A `blur` handler that reads
  // `document.activeElement` must see where focus went, not where it was --
  // and a handler that focuses a third element must win over this call rather
  // than be overwritten by it when it returns.
  // `blur`/`focus` do not bubble and `focusout`/`focusin` do, which is the
  // whole reason there are four events rather than two.
  if (previous != nullptr) {
    DispatchFocusEvent(*previous, "blur", false, target);
    DispatchFocusEvent(*previous, "focusout", true, target);
  }
  if (target != nullptr) {
    DispatchFocusEvent(*target, "focus", false, previous);
    DispatchFocusEvent(*target, "focusin", true, previous);
  }
  return true;
}

void DomBindings::DispatchFocusEvent(dom::Element& target, const char* type, bool bubbles,
                                     dom::Element* related) {
  if (interpreter_ == nullptr) {
    return;
  }
  // Not cancelable, and trusted: the only caller is the thing that saw the
  // focus move.
  const Value event = MakeEvent(type, bubbles, false, true);
  if (!event.IsObject()) {
    return;
  }
  const Value prototype = EventPrototype("FocusEvent", "UIEvent");
  if (prototype.IsObject()) {
    event.object->SetPrototype(prototype.object);
  }
  // The other end of the move. A page that highlights a row on `focusin` reads
  // it to know whether focus arrived from inside the row or from outside it.
  event.object->Set("relatedTarget", related == nullptr ? Value::Null() : WrapperFor(related));
  DispatchEventTo(target, event);
}

void DomBindings::InstallFocus(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->Set(name, native);
    }
  };

  // Measured at 232 calls across the survey, and reddit's front door needs one:
  // a form that is submitted programmatically focuses its field first.
  method("focus", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    // Not keyboard-driven. `focus()` from script is the case every browser
    // agreed *not* to show a focus ring for, because a page that focuses a
    // field on load would otherwise ring it before the user has touched
    // anything.
    owner->MoveFocus(static_cast<dom::Element*>(self), false);
    return Value::Undefined();
  });
  method("blur", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    // Only if it is the one that has focus. `other.blur()` blurring the
    // document's focused element would be a page able to take focus off
    // anything by naming anything.
    if (owner->FocusedElement() == self) {
      owner->MoveFocus(nullptr, false);
    }
    return Value::Undefined();
  });
  // HTMLElement.click() — measured everywhere consent UIs and form scripts
  // prefer a method call over fabricating a MouseEvent. Without it youtube's
  // Accept all path is unreachable from script, and feature detection that
  // expects a function throws instead of activating.
  method("click", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    auto& element = static_cast<dom::Element&>(*self);
    // Spec: disabled form controls do not activate. A bare `disabled` on a
    // custom element is still an attribute a page sets to mean "do not click".
    if (html::IsDisabledFormControl(element) || element.HasAttribute("disabled")) {
      return Value::Undefined();
    }
    PointerInput pointer;
    if (owner->geometry_ != nullptr) {
      if (const auto box = owner->geometry_->QueryBox(element)) {
        pointer.client_x = box->border_box.x + box->border_box.width * 0.5f;
        pointer.client_y = box->border_box.y + box->border_box.height * 0.5f;
        pointer.page_x = pointer.client_x;
        pointer.page_y = pointer.client_y;
      }
    }
    pointer.button = 0;
    pointer.buttons = 0;
    // **The activation behaviour is the engine's, and it is recorded rather
    // than run.** Toggling a checkbox, submitting a form, following an
    // `href` -- all of it lays out, navigates or repaints, none of which this
    // module may see. `engine::Page::ResolveClickActivation` already
    // implements every case for real pointer input; a second copy here is how
    // a click from script and a click from a mouse come to disagree. So the
    // element is left for the engine, exactly as a `requestSubmit()` is.
    //
    // Only when nothing cancelled: `preventDefault` in a handler is what
    // stops the default action, and that is the whole contract of a
    // cancelable click.
    if (!owner->DispatchClick(element, pointer)) {
      // Bounded: a page can call `click()` in a loop, and this list is drained
      // once per turn. Past the bound the activation is dropped rather than
      // the list grown, which is the same choice the live-range ring makes.
      constexpr std::size_t kMaxPendingActivations = 256;
      if (owner->pending_activations_.size() < kMaxPendingActivations) {
        owner->pending_activations_.push_back(&element);
      }
    }
    return Value::Undefined();
  });
}

dom::Element* DomBindings::FocusedElement() const {
  return document_ == nullptr ? nullptr : document_->Focus().element;
}

void DomBindings::InstallActiveElement(const js::Value& document) {
  if (!document.IsObject()) {
    return;
  }
  const Value native = interpreter_->NewNativeValue("activeElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    // The body when nothing is focused, which is what every engine reports and
    // what `document.activeElement === document.body` is written to test. Null
    // only in a document that has no body at all.
    dom::Element* focused = owner->FocusedElement();
    return owner->WrapperFor(focused != nullptr ? focused : owner->BodyElement());
  });
  if (native.IsObject()) {
    native.object->Set(kOwnerSlot, OwnerValue(this));
    document.object->DefineAccessor("activeElement", native.object, nullptr);
  }
}

dom::Element* DomBindings::BodyElement() const {
  return document_ == nullptr ? nullptr : document_->Body();
}

}  // namespace microbrowser::bindings
