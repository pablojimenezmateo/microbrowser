// `<iframe>`'s two accessors, and the windows around this one. ADR 0027 §1 for
// the element half, ADR 0042 §5 for the realm half.
//
// The comment this file used to open with said `contentWindow` could not be the
// child's global object, because each browsing context had its own
// `js::Interpreter` and therefore its own heap. That is no longer true: a
// same-origin child is a *realm* of the embedder's interpreter, so the two
// share a heap by construction and handing one's global to the other is an
// ordinary reference. The stub it left behind -- a plain object with a
// `document` on it -- is what `InstallFrameWindows` overwrites.

#include "bindings/FrameBindings.h"

#include <string>

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

// The interface table `DomBindings` publishes on the global. Under a name that
// is not an identifier, so a page cannot read it and cannot shadow it -- which
// is what makes reaching it from outside that class safe rather than a hole.
js::Object* InterfaceTable(js::Interpreter& interpreter) {
  js::Object* global = interpreter.Global();
  if (global == nullptr) {
    return nullptr;
  }
  const Value* interfaces = global->GetOwn("#domInterfaces");
  return interfaces != nullptr && interfaces->IsObject() ? interfaces->object : nullptr;
}

}  // namespace

js::Object* FrameGlobals::Nested(const dom::Element* element) const {
  if (element == nullptr) {
    return nullptr;
  }
  for (const auto& [key, global] : nested_) {
    if (key == element) {
      return global;
    }
  }
  return nullptr;
}

void FrameGlobals::SetNested(const dom::Element* element, js::Object* global) {
  if (element == nullptr || global == nullptr) {
    return;
  }
  for (auto& [key, existing] : nested_) {
    if (key == element) {
      // Overwritten rather than appended: `iframe.src = other` is a navigation,
      // and the same element then names a *different* realm. Appending would
      // leave `window.length` counting the frame twice and `window[0]` naming
      // the document that has already gone.
      existing = global;
      return;
    }
  }
  nested_.emplace_back(element, global);
}

js::Object* FrameGlobals::At(std::size_t index) const {
  return index < nested_.size() ? nested_[index].second : nullptr;
}

void FrameGlobals::Clear() {
  nested_.clear();
  embedder_ = nullptr;
  top_ = nullptr;
}

void InstallFrameWindows(js::Interpreter& interpreter, const FrameGlobals& globals) {
  js::Object* interfaces = InterfaceTable(interpreter);
  if (interfaces == nullptr) {
    return;
  }
  const Value* iframe_interface = interfaces->GetOwn("HTMLIFrameElement");
  if (iframe_interface == nullptr || !iframe_interface->IsObject()) {
    return;
  }
  const FrameGlobals* table = &globals;

  // Redefines what `InstallFrameElement` put here, and deliberately so: that one
  // answers with a plain object holding a `document`, which is all there was
  // before a child could have a realm. Two accessors for the same name is one
  // too many, and the last one installed is the one a page sees.
  const Value content_window =
      interpreter.NewNativeValue("contentWindow", [table](NativeCall& call) {
        dom::Element* element = ElementOf(call.self);
        js::Object* global = table->Nested(element);
        // Null rather than undefined, and for both reasons at once: it is what
        // the DOM answers for an absent context, and it is what a cross-origin
        // frame answers. A page cannot tell those apart, which is correct.
        return global == nullptr ? Value::Null() : Value::Obj(global);
      });
  if (content_window.IsObject()) {
    iframe_interface->object->DefineAccessor("contentWindow", content_window.object, nullptr);
  }

  // **`contentDocument` is `contentWindow.document`, not a second wrapper for
  // the same node.** Before this it was the *embedder's* wrapper for the child's
  // document, so `f.contentDocument === f.contentWindow.document` was false --
  // a page comparing them would conclude it was looking at two documents. Going
  // through the child's own global is what makes them one object without
  // needing a wrapper cache shared between two binding layers, which is the
  // larger decision ADR 0042 §5 step 3 leaves open.
  const Value content_document =
      interpreter.NewNativeValue("contentDocument", [table](NativeCall& call) {
        dom::Element* element = ElementOf(call.self);
        js::Object* global = table->Nested(element);
        if (global == nullptr) {
          return Value::Null();
        }
        const Value* document = global->GetOwn("document");
        return document != nullptr ? *document : Value::Null();
      });
  if (content_document.IsObject()) {
    iframe_interface->object->DefineAccessor("contentDocument", content_document.object, nullptr);
  }
}

void PublishFrameWindows(js::Interpreter& interpreter, const FrameGlobals& globals) {
  js::Object* global = interpreter.Global();
  if (global == nullptr) {
    return;
  }
  const Value window = Value::Obj(global);
  // A top-level document is its own parent and its own top, which is what HTML
  // says and what every `while (w !== w.parent)` walk terminates on.
  js::Object* embedder = globals.Embedder();
  js::Object* top = globals.Top();
  global->Set("parent", embedder != nullptr ? Value::Obj(embedder) : window);
  global->Set("top", top != nullptr ? Value::Obj(top) : window);

  // `window[0]` is the first child context and `window.length` is how many there
  // are; `frames` is the window itself, so `frames[0]` is the same lookup. Both
  // are rewritten rather than patched, because a frame that left the document
  // has to stop being addressable -- and the count falling is the only thing
  // that says so.
  const std::size_t count = globals.Count();
  for (std::size_t index = 0; index < count; ++index) {
    js::Object* child = globals.At(index);
    global->Set(std::to_string(index), child != nullptr ? Value::Obj(child) : Value::Undefined());
  }
  // One past the end, so a page that read `window[0]` and then removed the frame
  // does not keep finding it. Only one: the list only ever shrinks by removals
  // this same pass would have seen, and clearing an unbounded range would be a
  // page-controlled loop.
  global->Set(std::to_string(count), Value::Undefined());
  global->Set("length", Value::Number(static_cast<double>(count)));
}

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
    content_document.object->Set(kOwnerSlot, OwnerValue(owner));
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
    content_window.object->Set(kOwnerSlot, OwnerValue(owner));
    iframe_interface.object->DefineAccessor("contentWindow", content_window.object, nullptr);
  }
}

}  // namespace microbrowser::bindings
