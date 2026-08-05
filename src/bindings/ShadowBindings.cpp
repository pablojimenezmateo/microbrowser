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
    if (element.ShadowRoot() != nullptr) {
      // A second `attachShadow` is an error rather than a replacement: the page
      // holds references into the first tree, and swapping it silently would
      // strand them.
      Value error = call.interpreter.MakeError(
          "Error", "attachShadow: this element already has a shadow root");
      if (error.IsObject()) {
        error.object->Set("name", Value::String("NotSupportedError"));
      }
      return call.ThrowValue(error);
    }
    bool open = true;
    const Value options = Argument(call.arguments, 0);
    if (options.IsObject()) {
      if (const Value* mode = options.object->Get("mode")) {
        open = js::ToString(*mode) != "closed";
      }
    }
    dom::DocumentFragment* root = element.AttachShadow(open);
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
    attach.object->Set(kOwnerSlot, PointerValue(this));
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
    shadow_root.object->Set(kOwnerSlot, PointerValue(this));
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
    assigned_slot.object->Set(kOwnerSlot, PointerValue(this));
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
      method.object->Set(kOwnerSlot, PointerValue(this));
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

}  // namespace microbrowser::bindings
