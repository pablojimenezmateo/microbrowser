// `<iframe>`'s two accessors, and the reason the second one is not what its
// name promises. ADR 0027.

#include "bindings/FrameBindings.h"

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "dom/Node.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

dom::Element* ElementOf(const js::Value& value) {
  dom::Node* node = NodeOf(value);
  return node != nullptr && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr;
}

}  // namespace

void InstallFrameElement(DomBindings& owner_bindings, js::Interpreter& interpreter,
                         const js::Value& iframe_interface) {
  if (!iframe_interface.IsObject()) {
    return;
  }
  DomBindings* owner = &owner_bindings;

  const auto nested = [](NativeCall& call) -> dom::Document* {
    dom::Element* element = ElementOf(call.self);
    return element == nullptr ? nullptr : element->NestedDocument();
  };

  const Value content_document =
      interpreter.NewNativeValue("contentDocument", [nested](NativeCall& call) {
        DomBindings* target = OwnerOf(call);
        dom::Document* document = nested(call);
        if (target == nullptr || document == nullptr) {
          // Null rather than undefined, and for both reasons at once: it is what the DOM answers
          // for an absent node, and it is what a cross-origin frame answers. A page cannot tell the
          // two apart, which is correct -- "is there a document there" is itself information about
          // another origin.
          return Value::Null();
        }
        return target->WrapperFor(document);
      });
  if (content_document.IsObject()) {
    content_document.object->Set(kOwnerSlot, PointerValue(owner));
    iframe_interface.object->DefineAccessor("contentDocument", content_document.object, nullptr);
  }

  // `contentWindow` is deliberately **not** the child's global object.
  //
  // It cannot be: each browsing context has its own `js::Interpreter` and therefore its own heap,
  // which is what makes ADR 0027 §5's process split an extraction rather than a rewrite -- and an
  // object from one heap handed to another is a use-after-free waiting for the first collection.
  // What a same-origin page actually uses `contentWindow` for is `.document`, and that is answered
  // here; the full same-origin window -- a page reaching a global its own frame's script set --
  // needs a realm concept in `src/js`, which is written up in the session log as the next cost of
  // this ADR.
  //
  // Absent entirely for a cross-origin frame rather than a `WindowProxy`, which is a **known
  // deviation from ADR 0027 §2** and the reason `postMessage` across frames is not here yet.
  const Value content_window =
      interpreter.NewNativeValue("contentWindow", [nested](NativeCall& call) {
        DomBindings* target = OwnerOf(call);
        dom::Document* document = nested(call);
        if (target == nullptr || document == nullptr) {
          return Value::Null();
        }
        const Value window = call.interpreter.NewObjectValue();
        if (!window.IsObject()) {
          return Value::Null();
        }
        window.object->Set("document", target->WrapperFor(document));
        return window;
      });
  if (content_window.IsObject()) {
    content_window.object->Set(kOwnerSlot, PointerValue(owner));
    iframe_interface.object->DefineAccessor("contentWindow", content_window.object, nullptr);
  }
}

}  // namespace microbrowser::bindings
