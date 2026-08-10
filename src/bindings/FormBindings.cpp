// Forms: `document.forms`, `form.elements`, `submit()` and `requestSubmit()`.
//
// The distinction this file exists to get right is one line of the
// specification with a page-shaped consequence. `requestSubmit()` fires the
// `submit` event and runs validation; `submit()` does neither. A browser that
// implements the first as an alias for the second submits the form *without
// the fields the page's own handler was going to add* -- and reddit's
// interstitial is exactly that page: it assigns `form.onsubmit`, has the
// handler copy every query parameter into a hidden input, and then calls
// `requestSubmit()`. Aliased, the challenge is submitted with no answer in it
// and nothing anywhere reports a problem. See ADR 0026 §4.
//
// Form ownership comes from `html::FormOwner`, which is the same function the
// engine builds its form data set with. That is the reason this module's
// manifest now names `html`: `form.elements` answering with a descendant walk
// would be wrong for a control that names its form with `form="id"`, and wrong
// in the direction where two parts of the browser disagree about which
// controls a form owns.

#include <cstddef>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "html/FormControl.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;

bool IsFormElement(const dom::Node& node) {
  return node.IsElement() && static_cast<const dom::Element&>(node).TagName() == "form";
}

// The controls a form owns, in document order. "Listed" elements only, which
// is what `form.elements` contains: an `<img>` inside a form is not in it.
bool IsListedControl(const dom::Element& element) {
  const std::string& tag = element.TagName();
  return tag == "input" || tag == "select" || tag == "textarea" || tag == "button" ||
         tag == "fieldset" || tag == "output" || tag == "object";
}

}  // namespace

js::Value DomBindings::MakeNamedCollection(const std::vector<dom::Element*>& elements) {
  std::vector<Value> wrappers;
  wrappers.reserve(elements.size());
  for (dom::Element* element : elements) {
    wrappers.push_back(WrapperFor(element));
  }
  const Value collection = interpreter_->NewArrayValue(std::move(wrappers));
  if (!collection.IsObject()) {
    return collection;
  }
  // An array first, so `length`, indexing, `for...of` and spread all work
  // without a NodeList type nothing here could keep live anyway. What it is
  // not is live: it is what the tree looked like when it was asked for.
  for (dom::Element* element : elements) {
    // `name` first and `id` second, which is the order the specification's
    // named-getter uses. A name that collides with a method -- a control
    // called "submit" -- would shadow it, so those are left alone; the page
    // can still reach it by index or by `namedItem`.
    for (const char* attribute : {"name", "id"}) {
      const std::string* name = element->GetAttribute(attribute);
      if (name == nullptr || name->empty() || collection.object->GetOwn(*name) != nullptr) {
        continue;
      }
      collection.object->SetHidden(*name, WrapperFor(element));
    }
  }
  const Value named = interpreter_->NewNativeValue("namedItem", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Null();
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    const Value* found = call.self.object->GetOwn(wanted);
    // Null rather than undefined for a name nothing answers to, which is what
    // `elements.namedItem('x') === null` tests for.
    return found == nullptr || !found->IsObject() ? Value::Null() : *found;
  });
  if (named.IsObject()) {
    named.object->Set(kOwnerSlot, PointerValue(this));
    collection.object->SetHidden("namedItem", named);
  }
  return collection;
}

std::optional<PendingSubmit> DomBindings::TakePendingSubmit() {
  std::optional<PendingSubmit> taken;
  taken.swap(pending_submit_);
  return taken;
}

void DomBindings::InstallFormApis() {
  // `document.forms`, as an accessor so it follows the tree rather than
  // freezing what it looked like when the bindings were installed. reddit's
  // interstitial reads `document.forms[0]`.
  //
  // On the `Document` interface rather than on the document's wrapper, and not
  // as a matter of taste: this runs from EnsureInterfaces, which runs from the
  // *first* WrapperFor -- so asking for the document's wrapper here would build
  // a second one, install this on it, and have the outer call cache the first.
  // The accessor would have landed on an object nothing could reach.
  const Value* document_interface =
      interfaces_.IsObject() ? interfaces_.object->GetOwn("Document") : nullptr;

  const auto document_collection =
      [this, document_interface](const char* name, auto predicate) {
        const Value accessor = interpreter_->NewNativeValue(name, [predicate](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          if (owner == nullptr) {
            return Value::Undefined();
          }
          std::vector<dom::Element*> found;
          owner->ForEachElement([&](dom::Element& element) {
            if (predicate(element)) {
              found.push_back(&element);
            }
          });
          return owner->MakeNamedCollection(found);
        });
        if (accessor.IsObject() && document_interface != nullptr &&
            document_interface->IsObject()) {
          accessor.object->Set(kOwnerSlot, PointerValue(this));
          document_interface->object->DefineAccessor(name, accessor.object, nullptr);
        }
      };
  document_collection("forms",
                      [](const dom::Element& element) { return IsFormElement(element); });
  // `document.scripts` / `images` / `links` are the same shape as `forms`.
  // youtube's bootstrap and every framework that waits for "the page's own
  // scripts" read `document.scripts`; without it the name is `undefined` and
  // feature detection takes the branch written for browsers that have none.
  document_collection("scripts", [](const dom::Element& element) {
    return element.TagName() == "script";
  });
  document_collection("images", [](const dom::Element& element) {
    return element.TagName() == "img";
  });
  document_collection("links", [](const dom::Element& element) {
    return element.TagName() == "a" && element.GetAttribute("href") != nullptr;
  });

  const Value* form_interface = interfaces_.IsObject()
                                    ? interfaces_.object->GetOwn("HTMLFormElement")
                                    : nullptr;
  if (form_interface == nullptr || !form_interface->IsObject()) {
    return;
  }
  const Value form_prototype = *form_interface;

  const Value elements = interpreter_->NewNativeValue("elements", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    const auto& form = static_cast<const dom::Element&>(*self);
    std::vector<dom::Element*> found;
    owner->ForEachElement([&](dom::Element& element) {
      // Ownership, not containment: a control with `form="login"` belongs to
      // that form wherever it sits in the tree. Same function the engine's
      // form data set uses, so the two cannot disagree.
      if (IsListedControl(element) && html::BelongsToForm(element, form, *owner->document_)) {
        found.push_back(&element);
      }
    });
    return owner->MakeNamedCollection(found);
  });
  if (elements.IsObject()) {
    elements.object->Set(kOwnerSlot, PointerValue(this));
    form_prototype.object->DefineAccessor("elements", elements.object, nullptr);
  }

  const auto method = [this, &form_prototype](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      form_prototype.object->Set(name, native);
    }
  };

  // `submit()`: no `submit` event, no validation. That is not a shortcut, it
  // is the definition -- and it is why a page that wants its handler to run
  // has to call the other one.
  method("submit", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "submit called on a non-form");
    }
    owner->RecordSubmit(static_cast<dom::Element&>(*self), nullptr);
    return Value::Undefined();
  });

  // `requestSubmit(submitter)`: fires `submit`, which is the whole difference.
  method("requestSubmit", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "requestSubmit called on a non-form");
    }
    auto& form = static_cast<dom::Element&>(*self);
    dom::Element* submitter = nullptr;
    if (const Value candidate = Argument(call.arguments, 0); candidate.IsObject()) {
      dom::Node* node = NodeOf(candidate);
      if (node == nullptr || !node->IsElement()) {
        return call.Throw("TypeError", "the submitter must be an element");
      }
      auto& element = static_cast<dom::Element&>(*node);
      // Both checks the specification makes, and both throw rather than
      // quietly submitting something else.
      if (!html::IsSubmitControl(element)) {
        return call.Throw("TypeError", "the submitter must be a submit button");
      }
      if (!html::BelongsToForm(element, form, *owner->document_)) {
        return ThrowDom(call, "NotFoundError", "the submitter does not belong to this form");
      }
      submitter = &element;
    }
    // The event runs *now*, synchronously, because its handler is what adds
    // the fields the submission is supposed to carry. The submission itself
    // waits for the turn to end -- see PendingSubmit.
    if (owner->DispatchSubmit(form)) {
      return Value::Undefined();  // a handler called preventDefault
    }
    owner->RecordSubmit(form, submitter);
    return Value::Undefined();
  });

  // `control.form` -- the form a control belongs to, by the same ownership
  // rule. On Element rather than on each control interface, because the answer
  // for anything that is not a listed control is null and that is correct.
  const Value* element_interface =
      interfaces_.IsObject() ? interfaces_.object->GetOwn("Element") : nullptr;
  if (element_interface == nullptr || !element_interface->IsObject()) {
    return;
  }
  const Value owner_form = interpreter_->NewNativeValue("form", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Null();
    }
    const dom::Element* found =
        html::FormOwner(static_cast<const dom::Element&>(*self), *owner->document_);
    return owner->WrapperFor(const_cast<dom::Element*>(found));
  });
  if (owner_form.IsObject()) {
    owner_form.object->Set(kOwnerSlot, PointerValue(this));
    element_interface->object->DefineAccessor("form", owner_form.object, nullptr);
  }
}

void DomBindings::RecordSubmit(dom::Element& form, dom::Element* submitter) {
  if (pending_submit_.has_value()) {
    // The first one wins. A script that submits twice in one turn gets one
    // navigation, because the first is what tears this document down -- which
    // is what would have happened had the navigation been synchronous.
    return;
  }
  pending_submit_ = PendingSubmit{&form, submitter};
}

}  // namespace microbrowser::bindings
