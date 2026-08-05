// `document` itself: the methods and properties that hang off it rather than
// off a node.
//
// Split out because DomBindings.cpp reached the module's line cap, and the cap
// is written to mean a missing translation unit rather than a bigger file. The
// seam is a real one: everything here is *the document as an object* -- what a
// page reaches through the `document` global -- while the rest of the module
// is about nodes in general.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

void DomBindings::Install() {
  const Value document = WrapperFor(document_);
  if (!document.IsObject()) {
    return;
  }

  const auto method = [this, &document](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      document.object->Set(name, native);
    }
  };

  method("getElementById", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    const std::string wanted = js::ToString(Argument(call.arguments, 0));
    return owner->WrapperFor(owner->FindElement([&wanted](const dom::Element& element) {
      const std::string* id = element.GetAttribute("id");
      return id != nullptr && *id == wanted;
    }));
  });
  method("getElementsByTagName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string wanted = LowerCase(js::ToString(Argument(call.arguments, 0)));
    std::vector<Value> found;
    if (owner != nullptr) {
      owner->ForEachElement([&](dom::Element& element) {
        if (wanted == "*" || element.TagName() == wanted) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("querySelector", [](NativeCall& call) {
    // Tag, `#id` and `.class` only. A real selector engine exists in `src/css`
    // and this module is not allowed to see it -- widening `allow:` to reach it
    // would widen a security boundary for a convenience.
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    return owner->WrapperFor(owner->FindElement(
        [&selector](const dom::Element& element) { return Matches(element, selector); }));
  });
  method("querySelectorAll", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    std::vector<Value> found;
    if (owner != nullptr) {
      owner->ForEachElement([&](dom::Element& element) {
        if (Matches(element, selector)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    // An array, not a NodeList. A page indexes it, takes its length and
    // spreads it, and all three work -- what it does not get is the live
    // collection a NodeList is, which nothing here could keep up to date
    // anyway.
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("getElementsByClassName", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    const std::string selector = "." + js::ToString(Argument(call.arguments, 0));
    std::vector<Value> found;
    if (owner != nullptr) {
      owner->ForEachElement([&](dom::Element& element) {
        if (Matches(element, selector)) {
          found.push_back(owner->WrapperFor(&element));
        }
      });
    }
    return call.interpreter.NewArrayValue(std::move(found));
  });
  method("createTextNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null()
                            : owner->CreateText(js::ToString(Argument(call.arguments, 0)));
  });
  method("createElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElement(LowerCase(js::ToString(Argument(call.arguments, 0))));
  });
  // The namespace is accepted and ignored, which is honest for this parser:
  // it produces one tree with no XML in it, so `createElementNS(HTML_NS, 'div')`
  // and `createElement('div')` describe the same element. A page that asks for
  // a genuinely foreign namespace gets an HTML element and not a wrong answer
  // about one -- SVG is rendered from its own decoder, not from the DOM.
  method("createElementNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElement(LowerCase(js::ToString(Argument(call.arguments, 1))));
  });
  method("createDocumentFragment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->CreateDocumentFragment();
  });
  method("createComment", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateComment(js::ToString(Argument(call.arguments, 0)));
  });
  method("createEvent", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    return owner == nullptr ? Value::Null() : owner->CreateLegacyEvent();
  });
  // `readyState`, and now it moves.
  //
  // It used to answer "complete" always, on the reasoning that scripts run
  // after the parse here so reporting "loading" would be a lie. That was the
  // wrong half of the trade: the two states are read by pages that write
  // `if (readyState === 'loading') addEventListener('DOMContentLoaded', go);
  // else go()`, and answering "complete" while `DOMContentLoaded` had not yet
  // fired sent the *other* half of them -- the ones that only listen -- into a
  // wait for an event that never came. reddit's interstitial is one of those.
  //
  // So the lifecycle is real: "loading" while the scripts run, "interactive"
  // when DOMContentLoaded fires, "complete" when the load does. What is still
  // a deviation is *when* the scripts run relative to the parse, which
  // PageScript.h records.
  const Value ready = interpreter_->NewNativeValue("readyState", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::String(std::string("loading"));
    }
    const Value* state = call.self.object->GetOwn(kReadyStateSlot);
    return state == nullptr ? Value::String(std::string("loading")) : *state;
  });
  if (ready.IsObject()) {
    document.object->SetHidden(kReadyStateSlot, Value::String(std::string("loading")));
    document.object->DefineAccessor("readyState", ready.object, nullptr);
  }

  // `document.body` and `document.documentElement`, as accessors so they
  // follow the tree rather than freezing whatever it looked like at install.
  const auto element_accessor = [this, &document](const char* name, const char* tag) {
    const Value native =
        interpreter_->NewNativeValue(name, [tag](NativeCall& call) {
          DomBindings* owner = OwnerOf(call);
          return owner == nullptr ? Value::Null()
                                  : owner->WrapperFor(owner->FindElement(
                                        [tag](const dom::Element& element) {
                                          return element.TagName() == tag;
                                        }));
        });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      document.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  element_accessor("body", "body");
  element_accessor("documentElement", "html");
  InstallActiveElement(document);

  // `document.head`, `document.title` and the two element accessors, as
  // accessors so they follow the tree rather than freezing what it looked like
  // at install.
  element_accessor("head", "head");
  const Value title_getter = interpreter_->NewNativeValue("title", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::String(std::string());
    }
    dom::Element* title = owner->FindElement(
        [](const dom::Element& element) { return element.TagName() == "title"; });
    return Value::String(title == nullptr ? std::string() : title->TextContent());
  });
  if (title_getter.IsObject()) {
    title_getter.object->Set(kOwnerSlot, PointerValue(this));
    document.object->DefineAccessor("title", title_getter.object, nullptr);
  }

  interpreter_->GlobalScope()->Declare("document", document, false);
  InstallEventConstructors();
  InstallWindow();
}

void DomBindings::SetReadyState(const char* state) {
  const Value document = WrapperFor(document_);
  if (document.IsObject()) {
    document.object->SetHidden(kReadyStateSlot, Value::String(std::string(state)));
  }
}

}  // namespace microbrowser::bindings
