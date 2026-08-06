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
#include "util/PerformanceCounters.h"

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

  // Unresolved `[[prop]]` / `[prop]` / `{{prop}}` tokens in attributes are not
  // data — they are template binding syntax. Polymer's constructor reads
  // `this.attributes` and JSON.parses Array/Object types; a literal
  // `[[computedBadges]]` warns and leaves the property null, which is what
  // stopped youtube's badge subtree from stamping.
  for (const dom::Attribute& attribute : element.Attributes()) {
    if (IsUnresolvedTemplateBindingValue(attribute.value)) {
      element.RemoveAttribute(attribute.name);
    }
  }

  // The class's prototype goes on *before* the constructor runs, and that
  // ordering is the whole of whether a component works.
  //
  // A derived constructor's `this` comes from its base, and in a real engine
  // that object already has `new.target.prototype` installed by the time
  // `super()` returns -- so the very next line of the constructor may call the
  // class's own methods. Applying it afterwards instead makes `super()` hand
  // back a bare HTMLElement, and every constructor that calls one of its own
  // methods throws on the first one.
  //
  // Which is not a corner case: it is what a framework's base class *is*.
  // Polymer's begins `this._initializeProperties()`, and on youtube.com that
  // threw for twenty-nine of thirty-two upgrades, so no component ever
  // rendered and the page was blank. The element kept the prototype the failed
  // upgrade left it with, which is why it was not an instance of its own class.
  //
  // Setting it before is also what the specification says: the element's
  // prototype is set as part of constructing it, and a constructor that then
  // throws leaves the element "failed" without reverting anything.
  const Value* prototype = constructor->object->GetOwn("prototype");
  if (prototype != nullptr && prototype->IsObject()) {
    wrapper.object->SetPrototype(prototype->object);
  } else {
    util::AddPerformanceCounter(util::PerfCounterId::DomCustomElementPrototypeMissing);
  }

  // The handoff described at the top of this file: HTMLElement's constructor
  // returns whatever is parked here, so `super()` inside the page's class
  // yields the element that already exists.
  interfaces_.object->Set(kUpgradeSlot, wrapper);
  const js::Result constructed = interpreter_->ConstructValue(*constructor, {});
  interfaces_.object->Set(kUpgradeSlot, Value::Undefined());
  util::AddPerformanceCounter(util::PerfCounterId::DomCustomElementUpgrades);
  if (constructed.completion == js::Completion::Throw) {
    util::AddPerformanceCounter(util::PerfCounterId::DomCustomElementConstructorThrows);
    // A constructor that throws leaves the element in the tree as a plain
    // element rather than removing it, which is what a browser does: the
    // document is the page's, and a failed upgrade is not a reason to change
    // it.
    //
    // Reported, and it was not. The comment here said "the throw is reported
    // the way any uncaught one is" and nothing reported it: `ConstructValue`
    // hands the error back and this returned. On youtube.com that hid *thirty*
    // failing upgrades behind a blank page and an empty error list -- the
    // symptom looked like a component that never rendered, because that is
    // exactly what it was, and there was no way to see it from outside.
    // EventDispatch.cpp lost whole scripts to the same omission.
    interpreter_->ReportUncaught(constructed.value, "custom element constructor");
    return;
  }
  // Observed attributes present before upgrade: the specification queues one
  // attributeChangedCallback per attribute after construction, with a null old
  // value. Polymer's property effects may depend on this to apply bindings that
  // were not deserialized from literal attribute text.
  if (definition->object != nullptr) {
    const Value* observed = definition->object->GetOwn("observed");
    if (observed != nullptr && observed->IsObject()) {
      for (const dom::Attribute& attribute : element.Attributes()) {
        bool watched = false;
        for (std::size_t i = 0; i < observed->object->ElementCount(); ++i) {
          watched = watched || js::ToString(observed->object->GetElement(i)) == attribute.name;
        }
        if (watched) {
          RunAttributeReaction(element, attribute.name, Value::Null(), Value::String(attribute.value));
        }
      }
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
  // Reported rather than discarded, for the reason the constructor's throw now
  // is: a reaction that throws is how a component stops halfway, and a silent
  // one is indistinguishable from a component that had nothing to do.
  const bool was_trusted = trusted_script_insertion_;
  if (csp_script_strict_dynamic_) {
    trusted_script_insertion_ = true;
  }
  const js::Result ran = interpreter_->CallFunction(*handler, wrapper, {});
  trusted_script_insertion_ = was_trusted;
  if (ran.completion == js::Completion::Throw) {
    interpreter_->ReportUncaught(ran.value, "custom element reaction");
  }
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
  const js::Result ran = interpreter_->CallFunction(
      *handler, wrapper, {Value::String(name), old_value, new_value, Value::Null()});
  if (ran.completion == js::Completion::Throw) {
    interpreter_->ReportUncaught(ran.value, "attributeChangedCallback");
  }
}

void DomBindings::InstallCustomElements() {
  const Value registry = CustomElementRegistry();
  const Value api = interpreter_->NewObjectValue();
  if (!registry.IsObject() || !api.IsObject()) {
    return;
  }
  // `window.customElements` is an instance of `CustomElementRegistry`, and the
  // methods go on the interface rather than on the instance. That is where the
  // specification puts them, and it is where a polyfill replaces one:
  // youtube's webcomponents bundle assigns `window.customElements.define` and
  // then `Object.defineProperty(window.CustomElementRegistry.prototype,
  // "define", ...)` -- the second line is a TypeError in a browser that has the
  // registry but not its type.
  const Value prototype = MakeInterface("CustomElementRegistry", Value::Undefined());
  if (prototype.IsObject()) {
    api.object->SetPrototype(prototype.object);
  }
  const Value target = prototype.IsObject() ? prototype : api;
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
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
