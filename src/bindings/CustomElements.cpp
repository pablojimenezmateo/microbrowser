// `customElements` — the registry, the upgrade, and the reactions.
//
// Natively rather than through the web components polyfill, and the amendment
// to ADR 0012 records why that changed: the polyfill turned out to want
// `Range`, `TreeWalker`, `NodeIterator`, `ShadowRoot` and ten more interfaces,
// which is not a polyfill filling a gap but one reimplementing the DOM. What
// is here needs none of those.
//
// The mechanism that makes upgrading possible is the one every engine uses and
// it is worth stating, because it looks like a trick: a derived class's `this`
// is whatever its base constructor produced, so `class X extends HTMLElement`
// running `super()` takes its object *from HTMLElement*. Upgrading an element
// therefore means putting the element's existing wrapper where HTMLElement's
// constructor will return it, and then constructing the class normally. The
// element script gets is the element the document already had, rather than a
// second object that has to be kept in step with it.

#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// Where the registry and the in-flight upgrade live. Kept as properties of the
// interfaces object rather than as members, because that object is already
// rooted and already the place per-interface state belongs -- and because a
// C++ `js::Value` member is invisible to the collector unless something roots
// it, which is a bug this module has already had once.
constexpr const char* kRegistrySlot = "#customElements";
constexpr const char* kUpgradeSlot = "#upgrading";
// On a wrapper, once its class has run. An element is upgraded at most once.
constexpr const char* kUpgradedSlot = "#upgraded";

// A custom element name has a dash. That is the whole rule, and it is what
// keeps a page from redefining `<div>`.
bool IsValidCustomElementName(const std::string& name) {
  return !name.empty() && name.front() != '-' && name.find('-') != std::string::npos;
}

}  // namespace

js::Value DomBindings::PendingUpgrade() {
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  const Value* pending = interfaces_.object->GetOwn(kUpgradeSlot);
  return pending == nullptr ? Value::Undefined() : *pending;
}

js::Value DomBindings::CustomElementRegistry() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return Value::Undefined();
  }
  if (const Value* existing = interfaces_.object->GetOwn(kRegistrySlot)) {
    return *existing;
  }
  const Value registry = interpreter_->NewObjectValue();
  if (registry.IsObject()) {
    interfaces_.object->Set(kRegistrySlot, registry);
  }
  return registry;
}

void DomBindings::UpgradeElement(dom::Element& element) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject()) {
    return;
  }
  const Value* definition = registry.object->GetOwn(element.TagName());
  if (definition == nullptr || !definition->IsObject()) {
    return;  // not a custom element, which is the overwhelmingly common case
  }
  const Value wrapper = WrapperFor(&element);
  if (!wrapper.IsObject()) {
    return;
  }
  // At most once. A page that defines a name, appends an element, removes it
  // and appends it again must not run the constructor twice.
  if (wrapper.object->GetOwn(kUpgradedSlot) != nullptr) {
    return;
  }
  const Value* constructor = definition->object->GetOwn("constructor");
  if (constructor == nullptr || !constructor->IsObject() ||
      !constructor->object->IsCallable()) {
    return;
  }
  wrapper.object->Set(kUpgradedSlot, Value::Bool(true));

  // The handoff described at the top of this file: HTMLElement's constructor
  // returns whatever is parked here, so `super()` inside the page's class
  // yields the element that already exists.
  interfaces_.object->Set(kUpgradeSlot, wrapper);
  const js::Result constructed = interpreter_->ConstructValue(*constructor, {});
  interfaces_.object->Set(kUpgradeSlot, Value::Undefined());
  if (constructed.completion == js::Completion::Throw) {
    // A constructor that throws leaves the element in the tree as a plain
    // element rather than removing it, which is what a browser does: the
    // document is the page's, and a failed upgrade is not a reason to change
    // it. The throw is reported the way any uncaught one is.
    return;
  }
  // The prototype comes from the class whether or not its constructor set
  // anything, so a class body with only methods still works.
  if (const Value* prototype = constructor->object->GetOwn("prototype")) {
    if (prototype->IsObject()) {
      wrapper.object->SetPrototype(prototype->object);
    }
  }
  // Already in the document at upgrade time means connected now.
  for (const dom::Node* at = &element; at != nullptr; at = at->Parent()) {
    if (at == document_) {
      RunElementReaction(element, "connectedCallback");
      break;
    }
  }
}

void DomBindings::RunElementReaction(dom::Element& element, const char* callback) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject() || registry.object->GetOwn(element.TagName()) == nullptr) {
    return;  // not custom: no reaction to run, and no wrapper to make either
  }
  const Value wrapper = WrapperFor(&element);
  if (!wrapper.IsObject() || wrapper.object->GetOwn(kUpgradedSlot) == nullptr) {
    return;
  }
  const Value* handler = wrapper.object->Get(callback);
  if (handler == nullptr || !handler->IsObject() || !handler->object->IsCallable()) {
    return;  // a class need not define every reaction
  }
  (void)interpreter_->CallFunction(*handler, wrapper, {});
}

void DomBindings::RunAttributeReaction(dom::Element& element, const std::string& name,
                                       const js::Value& old_value, const js::Value& new_value) {
  const Value registry = CustomElementRegistry();
  if (!registry.IsObject()) {
    return;
  }
  const Value* definition = registry.object->GetOwn(element.TagName());
  if (definition == nullptr || !definition->IsObject()) {
    return;
  }
  // `observedAttributes` is read once, when the class is defined, and not on
  // every attribute write. That is the specification's rule and it is also the
  // one that keeps a setAttribute in a loop from calling into script.
  const Value* observed = definition->object->GetOwn("observed");
  if (observed == nullptr || !observed->IsObject()) {
    return;
  }
  bool watched = false;
  for (std::size_t i = 0; i < observed->object->ElementCount(); ++i) {
    watched = watched || js::ToString(observed->object->GetElement(i)) == name;
  }
  if (!watched) {
    return;
  }
  const Value wrapper = WrapperFor(&element);
  if (!wrapper.IsObject() || wrapper.object->GetOwn(kUpgradedSlot) == nullptr) {
    return;
  }
  const Value* handler = wrapper.object->Get("attributeChangedCallback");
  if (handler == nullptr || !handler->IsObject() || !handler->object->IsCallable()) {
    return;
  }
  (void)interpreter_->CallFunction(*handler, wrapper,
                                   {Value::String(name), old_value, new_value,
                                    Value::Null()});
}

void DomBindings::InstallCustomElements() {
  const Value registry = CustomElementRegistry();
  const Value api = interpreter_->NewObjectValue();
  if (!registry.IsObject() || !api.IsObject()) {
    return;
  }
  const auto method = [this, &api](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      api.object->Set(name, native);
    }
  };

  method("define", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Undefined();
    }
    const std::string name = LowerCase(js::ToString(Argument(call.arguments, 0)));
    const Value constructor = Argument(call.arguments, 1);
    if (!IsValidCustomElementName(name)) {
      // The dash rule, enforced rather than assumed: without it a page could
      // redefine `div` and every element in the document would be upgraded.
      return call.Throw("SyntaxError", "'" + name + "' is not a valid custom element name");
    }
    if (!constructor.IsObject() || !constructor.object->IsCallable()) {
      return call.Throw("TypeError", "the second argument must be a constructor");
    }
    const Value registry_object = owner->CustomElementRegistry();
    if (!registry_object.IsObject()) {
      return Value::Undefined();
    }
    if (registry_object.object->GetOwn(name) != nullptr) {
      return call.Throw("NotSupportedError", "'" + name + "' is already defined");
    }
    const Value definition = call.interpreter.NewObjectValue();
    if (!definition.IsObject()) {
      return Value::Undefined();
    }
    definition.object->Set("constructor", constructor);
    // Read once, here, which is both the rule and what keeps a setAttribute
    // loop from calling into script to ask.
    // Through a real property get, because `observedAttributes` is almost
    // always written as a `static get` -- reading the slot directly finds the
    // accessor rather than running it, and the list comes back undefined.
    definition.object->Set("observed",
                           call.interpreter.GetPropertyValue(constructor, "observedAttributes"));
    registry_object.object->Set(name, definition);

    // Everything already in the document with this name is upgraded now. A
    // page that defines its classes after its markup -- which is the usual
    // order, since the script is at the end of the body -- depends on this.
    // The list is taken before any constructor runs, because a constructor is
    // allowed to change the tree.
    std::vector<dom::Element*> pending;
    owner->ForEachElement([&](dom::Element& element) {
      if (element.TagName() == name) {
        pending.push_back(&element);
      }
    });
    for (dom::Element* element : pending) {
      owner->UpgradeElement(*element);
    }
    return Value::Undefined();
  });

  method("get", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Undefined();
    }
    const Value registry_object = owner->CustomElementRegistry();
    if (!registry_object.IsObject()) {
      return Value::Undefined();
    }
    const Value* definition =
        registry_object.object->GetOwn(LowerCase(js::ToString(Argument(call.arguments, 0))));
    if (definition == nullptr || !definition->IsObject()) {
      return Value::Undefined();
    }
    const Value* constructor = definition->object->GetOwn("constructor");
    return constructor == nullptr ? Value::Undefined() : *constructor;
  });

  if (api.IsObject()) {
    interpreter_->Global()->Set("customElements", api);
    interpreter_->GlobalScope()->Declare("customElements", api, false);
  }
}

}  // namespace microbrowser::bindings
