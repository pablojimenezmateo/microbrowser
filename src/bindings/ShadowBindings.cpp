// `attachShadow`, `shadowRoot`, and what a `<slot>` answers.
//
// ADR 0019 §1-2 and §5. Three things here are the design rather than the code:
//
//   * **`{mode: "closed"}` works and is not a security boundary**, and this file
//     will not pretend otherwise. The engine holds the root either way -- any
//     script that could call `attachShadow` could have kept the reference -- and
//     what closed changes is exactly one thing: `element.shadowRoot` answers
//     null. It is implemented because pages use it and *detect* it.
//   * **The flat tree is not built here.** Layout and the cascade walk it through
//     `dom::FlatChildren`, computed on demand, so there is no second tree for
//     this layer to keep in step. `assignedNodes()` asks the same function a
//     paint does.
//   * **`slotchange` needs the previous answer**, because it fires on a *change*.
//     That is the one piece of state ADR 0019 §2 admits into the design, and it
//     lives on the slot's wrapper rather than on the node -- the node tree must
//     not carry a fact that exists only because a page might be listening.

#include <cstddef>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/ShadowDom.h"
#include "dom/FlatTree.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Value;
using util::AddPerformanceCounter;
using util::PerfCounterId;

// The assignment this slot last reported, as a list of node pointers rendered as
// numbers. Compared rather than the nodes themselves, because a `slotchange`
// fires when the *set* changed and a comparison needs something to compare.
constexpr const char* kAssignmentSlot = "#assignment";

std::string AssignmentKey(const std::vector<dom::Node*>& nodes) {
  std::string key;
  for (dom::Node* node : nodes) {
    key += std::to_string(reinterpret_cast<std::uintptr_t>(node));
    key += ',';
  }
  return key;
}

}  // namespace

void DomBindings::InstallShadowDom(const js::Value& element_interface) {
  if (!element_interface.IsObject() || interpreter_ == nullptr) {
    return;
  }

  const Value attach = interpreter_->NewNativeValue("attachShadow", [this](NativeCall& call) {
    dom::Node* node = NodeOf(call.self);
    if (node == nullptr || !node->IsElement()) {
      return call.Throw("TypeError", "attachShadow called on something that is not an element");
    }
    auto& element = static_cast<dom::Element&>(*node);
    dom::ShadowFlags flags = dom::ShadowFlags::None;
    const Value options = Argument(call.arguments, 0);
    if (options.IsObject()) {
      // `mode` is required and its only two values are "open" and "closed".
      // Anything else is a TypeError from the IDL enumeration before any of this
      // runs, which is why an unrecognised mode must not quietly mean "open".
      const Value* mode = options.object->Get("mode");
      const std::string mode_text = mode == nullptr ? std::string() : js::ToString(*mode);
      if (mode_text != "open" && mode_text != "closed") {
        return call.Throw("TypeError",
                          "attachShadow: mode must be either \"open\" or \"closed\"");
      }
      if (mode_text == "open") {
        flags |= dom::ShadowFlags::Open;
      }
      const auto boolean_option = [&options](const char* name) {
        const Value* value = options.object->Get(name);
        return value != nullptr && js::ToBoolean(*value);
      };
      if (boolean_option("delegatesFocus")) {
        flags |= dom::ShadowFlags::DelegatesFocus;
      }
      if (boolean_option("clonable")) {
        flags |= dom::ShadowFlags::Clonable;
      }
      if (boolean_option("serializable")) {
        flags |= dom::ShadowFlags::Serializable;
      }
      if (const Value* slots = options.object->Get("slotAssignment");
          slots != nullptr && js::ToString(*slots) == "manual") {
        flags |= dom::ShadowFlags::ManualSlotAssignment;
      }
    } else {
      return call.Throw("TypeError", "attachShadow: an init dictionary with a mode is required");
    }

    const dom::ShadowAttachResult attached = element.AttachShadow(flags);
    if (attached.status == dom::ShadowAttachStatus::Refused) {
      // Two different refusals with one name, which is what the DOM specifies:
      // an element that cannot host a shadow root, and a host that already has
      // one this call does not match.
      return ThrowDom(call, "NotSupportedError",
                      "attachShadow: this element cannot have a shadow root attached");
    }
    if (attached.status == dom::ShadowAttachStatus::Reused) {
      // The declarative case. The DOM says to empty the root, and this is the
      // layer that can: `dom` may not free a node script might hold a wrapper
      // for, so `Element::AttachShadow` deliberately left the children behind.
      ClearChildren(*attached.root, true);
    }
    dom::DocumentFragment* root = attached.root;
    if (root == nullptr) {
      return Value::Undefined();
    }
    AddPerformanceCounter(PerfCounterId::DomShadowRootsAttached);
    // The document changed shape *for rendering* even though no node moved, so
    // the box tree and the cascade derived from it are stale. This is what ADR
    // 0015's layout-clean comparison reads, and attaching a root is the one
    // mutation that does not go through the five primitives that mark it.
    if (document_ != nullptr) {
      document_->NoteTreeMutation();
    }
    return WrapperFor(root);
  });
  if (attach.IsObject()) {
    attach.object->Set(kOwnerSlot, OwnerValue(this));
    element_interface.object->Set("attachShadow", attach);
  }

  const Value shadow_root = interpreter_->NewNativeValue("shadowRoot", [this](NativeCall& call) {
    dom::Node* node = NodeOf(call.self);
    if (node == nullptr || !node->IsElement()) {
      return Value::Null();
    }
    const auto& element = static_cast<const dom::Element&>(*node);
    // Null for a closed root, which is the *whole* of what closed mode does.
    if (element.ShadowRoot() == nullptr || !element.ShadowIsOpen()) {
      return Value::Null();
    }
    return WrapperFor(element.ShadowRoot());
  });
  if (shadow_root.IsObject()) {
    shadow_root.object->Set(kOwnerSlot, OwnerValue(this));
    element_interface.object->DefineAccessor("shadowRoot", shadow_root.object, nullptr);
  }

  // `assignedSlot`: which slot a light-DOM child renders in, or null. A page uses
  // it to ask "am I being slotted", which is a different question from "is my
  // parent a host".
  const Value assigned_slot = interpreter_->NewNativeValue("assignedSlot", [this](NativeCall& call) {
    dom::Node* node = NodeOf(call.self);
    if (node == nullptr || node->Parent() == nullptr || !node->Parent()->IsElement()) {
      return Value::Null();
    }
    const auto& host = static_cast<const dom::Element&>(*node->Parent());
    dom::DocumentFragment* root = host.ShadowRoot();
    if (root == nullptr) {
      return Value::Null();
    }
    // Walked rather than stored, for the reason the flat tree is a traversal: a
    // stored answer is one more thing that can disagree with the tree.
    Value found = Value::Null();
    root->ForEachDescendant([&](const dom::Node& candidate) {
      if (!found.IsNull() || !candidate.IsElement()) {
        return;
      }
      const auto& slot = static_cast<const dom::Element&>(candidate);
      if (slot.TagName() != "slot") {
        return;
      }
      for (dom::Node* assigned : dom::AssignedNodes(slot)) {
        if (assigned == node) {
          found = WrapperFor(const_cast<dom::Element*>(&slot));
          return;
        }
      }
    });
    return found;
  });
  if (assigned_slot.IsObject()) {
    assigned_slot.object->Set(kOwnerSlot, OwnerValue(this));
    element_interface.object->DefineAccessor("assignedSlot", assigned_slot.object, nullptr);
  }

  // `assignedNodes()` and `assignedElements()`, on Element rather than on a
  // separate HTMLSlotElement interface: the per-tag interfaces exist, but a
  // method that answers an empty list for a non-slot is what every polyfill
  // detection expects and is what the flat traversal already says.
  const auto install_assigned = [this, &element_interface](const char* name, bool elements_only) {
    const Value method =
        interpreter_->NewNativeValue(name, [this, elements_only](NativeCall& call) {
          dom::Node* node = NodeOf(call.self);
          std::vector<Value> out;
          if (node != nullptr && node->IsElement()) {
            const auto& slot = static_cast<const dom::Element&>(*node);
            // `flatten: true` would follow a slot assigned to another slot. Not
            // implemented, and absent rather than ignored: a page that passes it
            // and gets the unflattened list has a bug it cannot see, so the
            // option is simply not read and the answer is the direct assignment.
            for (dom::Node* assigned : dom::AssignedNodes(slot)) {
              if (elements_only && !assigned->IsElement()) {
                continue;
              }
              out.push_back(WrapperFor(assigned));
            }
          }
          return call.interpreter.NewArrayValue(std::move(out));
        });
    if (method.IsObject()) {
      method.object->Set(kOwnerSlot, OwnerValue(this));
      element_interface.object->Set(name, method);
    }
  };
  install_assigned("assignedNodes", false);
  install_assigned("assignedElements", true);
}

bool DomBindings::DeliverSlotChanges() {
  if (interpreter_ == nullptr || document_ == nullptr) {
    return false;
  }
  // Every slot in every shadow tree on the document. Walked rather than kept as a
  // list, because a slot is added and removed by ordinary tree mutation and a
  // list would be one more thing to keep correct at five call sites.
  std::vector<dom::Element*> slots;
  document_->ForEachDescendant([&slots](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.ShadowRoot() == nullptr) {
      return;
    }
    element.ShadowRoot()->ForEachDescendant([&slots](const dom::Node& inner) {
      if (inner.IsElement() &&
          static_cast<const dom::Element&>(inner).TagName() == "slot") {
        slots.push_back(const_cast<dom::Element*>(&static_cast<const dom::Element&>(inner)));
      }
    });
  });

  bool fired = false;
  for (dom::Element* slot : slots) {
    const Value wrapper = WrapperFor(slot);
    if (!wrapper.IsObject()) {
      continue;
    }
    const std::string key = AssignmentKey(dom::AssignedNodes(*slot));
    const Value* previous = wrapper.object->GetOwn(kAssignmentSlot);
    const bool first_time = previous == nullptr;
    if (!first_time && js::ToString(*previous) == key) {
      continue;
    }
    wrapper.object->SetHidden(kAssignmentSlot, Value::String(key));
    if (first_time) {
      // The first answer is not a *change*. A `slotchange` on the initial
      // assignment would fire on every page load for every slot, which is not
      // what a page listening for one is waiting for.
      continue;
    }
    const Value event = MakeEvent("slotchange", /*bubbles=*/true, /*cancelable=*/false,
                                  /*trusted=*/true);
    if (!event.IsObject()) {
      continue;
    }
    event.object->Set("target", wrapper);
    RunListenersOn(wrapper, event, "#on:slotchange", EventPhase::AtTarget);
    AddPerformanceCounter(PerfCounterId::DomSlotChanges);
    fired = true;
  }
  if (fired) {
    interpreter_->DrainMicrotasks();
  }
  return fired;
}

// `attachInternals()`, and **only the part of ElementInternals that exists**.
//
// The one thing behind it here is `shadowRoot`, which answers with the host's
// root whether it is open or closed -- that is the whole point of the DOM's
// "available to element internals" flag, and it is how a custom element reaches
// the closed root the parser attached on its behalf from
// `<template shadowrootmode=closed>`.
//
// Everything else an ElementInternals has -- `setFormValue`, `setValidity`,
// `form`, `willValidate`, the ARIA reflections -- is **absent rather than
// stubbed**, which is ADR 0012's rule and the same call `response.body` got.
// There is no form-association machinery under this, and a `setFormValue` that
// accepted a value and dropped it would send a page down the native path into a
// wall; a missing name sends it somewhere that works. `'setFormValue' in
// internals` is the check, and here it is honestly false.
void InstallElementInternals(DomBindings& owner_bindings, js::Interpreter& interpreter,
                             const js::Value& html_element_interface) {
  if (!html_element_interface.IsObject()) {
    return;
  }
  // Where an element's internals live once made. On the *wrapper* rather than in
  // a table, because "has attachInternals been called on this element" is
  // per-element state and the wrapper is this module's only per-element place.
  constexpr const char* kInternalsSlot = "#elementInternals";

  const Value attach = interpreter.NewNativeValue("attachInternals", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* node = NodeOf(call.self);
    if (owner == nullptr || node == nullptr || !node->IsElement()) {
      return call.Throw("TypeError", "attachInternals called on something that is not an element");
    }
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    if (const Value* existing = call.self.object->GetOwn(kInternalsSlot)) {
      // Twice on the same element is the specification's NotSupportedError, and
      // it is worth throwing: two ElementInternals for one element would be two
      // objects a page believes are the same one.
      (void)existing;
      return ThrowDom(call, "NotSupportedError",
                      "attachInternals: internals have already been attached to this element");
    }
    // `behaviors` is validated even though nothing here implements one, and that
    // is not busywork: the entries must be platform behavior objects
    // (`HTMLSubmitButtonBehavior` and friends), this browser defines none, so
    // *every* non-empty array is a TypeError and saying so is the correct
    // answer rather than a placeholder. Without it a page hands over a behavior,
    // gets no error, and finds out it did nothing at submit time.
    const Value options = Argument(call.arguments, 0);
    if (options.IsObject()) {
      if (const Value* behaviors = options.object->Get("behaviors");
          behaviors != nullptr && behaviors->IsObject() &&
          behaviors->object->ElementCount() != 0) {
        return call.Throw("TypeError",
                          "attachInternals: no element behaviors are implemented, so every "
                          "entry in `behaviors` is invalid");
      }
    }
    const Value internals = call.interpreter.NewObjectValue();
    if (!internals.IsObject()) {
      return Value::Undefined();
    }
    const Value host = call.self;
    const Value shadow_root =
        call.interpreter.NewNativeValue("shadowRoot", [host](NativeCall& inner) {
          DomBindings* bindings = OwnerOf(inner);
          dom::Node* element = NodeOf(host);
          if (bindings == nullptr || element == nullptr || !element->IsElement()) {
            return Value::Null();
          }
          // No open/closed test, unlike `element.shadowRoot`: internals are the
          // element's own view of itself, and a closed root is exactly what a
          // page reaches for through here.
          dom::DocumentFragment* root = static_cast<dom::Element&>(*element).ShadowRoot();
          return root == nullptr ? Value::Null() : bindings->WrapperFor(root);
        });
    if (shadow_root.IsObject()) {
      shadow_root.object->Set(kOwnerSlot, OwnerValue(owner));
      internals.object->DefineAccessor("shadowRoot", shadow_root.object, nullptr);
    }
    call.self.object->SetHidden(kInternalsSlot, internals);
    return internals;
  });
  if (attach.IsObject()) {
    attach.object->Set(kOwnerSlot, OwnerValue(&owner_bindings));
    html_element_interface.object->Set("attachInternals", attach);
  }
}

void InstallTemplateShadowReflection(DomBindings& owner_bindings, js::Interpreter& interpreter,
                                    const js::Value& template_interface) {
  if (!template_interface.IsObject()) {
    return;
  }
  const auto define = [&owner_bindings, &interpreter, &template_interface](const char* name, js::NativeFunction get,
                                                  js::NativeFunction set) {
    const Value getter = interpreter.NewNativeValue(name, std::move(get));
    const Value setter = interpreter.NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(&owner_bindings));
      setter.object->Set(kOwnerSlot, OwnerValue(&owner_bindings));
      template_interface.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  // `shadowRootMode` reflects an enumerated attribute with no invalid-value
  // default, so anything that is not "open" or "closed" -- including a missing
  // attribute -- reads back as the empty string. The *setter* stores what it was
  // given verbatim: `t.shadowRootMode = 'blah'` leaves `shadowrootmode="blah"`
  // in the markup and reads back "", which is what reflection means and what the
  // suite checks in both directions.
  define(
      "shadowRootMode",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr || !self->IsElement()) {
          return Value::String("");
        }
        const std::string* value =
            static_cast<dom::Element&>(*self).GetAttribute("shadowrootmode");
        if (value == nullptr) {
          return Value::String("");
        }
        const std::string folded = util::AsciiLowerCase(*value);
        return Value::String(folded == "open" || folded == "closed" ? folded : std::string());
      },
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self != nullptr && self->IsElement()) {
          static_cast<dom::Element&>(*self).SetAttribute("shadowrootmode",
                                                         js::ToString(Argument(call.arguments, 0)));
        }
        return Value::Undefined();
      });

  // `shadowRootSlotAssignment` is enumerated like `shadowRootMode`, but with an
  // invalid-value *default* rather than an empty one: anything that is not
  // "manual" -- including a missing attribute, an empty string and a misspelling
  // -- is "named". The setter stores what it was given, same as mode's.
  define(
      "shadowRootSlotAssignment",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr || !self->IsElement()) {
          return Value::String("named");
        }
        const std::string* value =
            static_cast<dom::Element&>(*self).GetAttribute("shadowrootslotassignment");
        const bool manual = value != nullptr && util::AsciiLowerCase(*value) == "manual";
        return Value::String(manual ? "manual" : "named");
      },
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self != nullptr && self->IsElement()) {
          static_cast<dom::Element&>(*self).SetAttribute("shadowrootslotassignment",
                                                         js::ToString(Argument(call.arguments, 0)));
        }
        return Value::Undefined();
      });

  // `shadowRootAdoptedStyleSheets` is a plain DOMString reflection -- no
  // enumeration, no whitespace normalisation, no splitting. The empty string
  // when the attribute is absent, and verbatim otherwise, which is what makes
  // `"  foo   bar  "` come back with its spaces.
  define(
      "shadowRootAdoptedStyleSheets",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr || !self->IsElement()) {
          return Value::String("");
        }
        const std::string* value =
            static_cast<dom::Element&>(*self).GetAttribute("shadowrootadoptedstylesheets");
        // Absent reads as "" and must not create the attribute on the way past:
        // a getter with a side effect on the tree is a getter a page can use to
        // change the document by reading it.
        return Value::String(value == nullptr ? std::string() : *value);
      },
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self != nullptr && self->IsElement()) {
          static_cast<dom::Element&>(*self).SetAttribute("shadowrootadoptedstylesheets",
                                                         js::ToString(Argument(call.arguments, 0)));
        }
        return Value::Undefined();
      });

  // The three boolean reflections. Present means true whatever the value, which
  // is why `shadowrootclonable="foobar"` is still clonable; assigning false
  // removes the attribute rather than setting it to "false".
  static constexpr struct {
    const char* property;
    const char* attribute;
  } kBooleans[] = {
      {"shadowRootDelegatesFocus", "shadowrootdelegatesfocus"},
      {"shadowRootClonable", "shadowrootclonable"},
      {"shadowRootSerializable", "shadowrootserializable"},
  };
  for (const auto& entry : kBooleans) {
    const char* attribute = entry.attribute;
    define(
        entry.property,
        [attribute](NativeCall& call) {
          dom::Node* self = NodeOf(call.self);
          return Value::Bool(self != nullptr && self->IsElement() &&
                             static_cast<dom::Element&>(*self).HasAttribute(attribute));
        },
        [attribute](NativeCall& call) {
          dom::Node* self = NodeOf(call.self);
          if (self == nullptr || !self->IsElement()) {
            return Value::Undefined();
          }
          auto& element = static_cast<dom::Element&>(*self);
          if (js::ToBoolean(Argument(call.arguments, 0))) {
            element.SetAttribute(attribute, "");
          } else {
            element.RemoveAttribute(attribute);
          }
          return Value::Undefined();
        });
  }
}

}  // namespace microbrowser::bindings
