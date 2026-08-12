#include "bindings/DomBindings.h"
#include "bindings/LiveRanges.h"

#include "bindings/BindingSupport.h"
#include "bindings/WebIdl.h"
#include "css/StyleSheet.h"
#include "dom/FlatTree.h"
#include "util/Env.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Object;
using js::Value;

// One entry per binding layer that currently exists. See `OwnerValue` in
// BindingSupport.h for why the key is a serial rather than the address.
struct LiveOwner {
  std::uint64_t serial = 0;
  DomBindings* bindings = nullptr;
};

std::vector<LiveOwner>& LiveOwners() {
  static std::vector<LiveOwner> live;
  return live;
}

// Never reused, which is the whole point: a stale native must find nothing
// rather than whatever was built at the same address afterwards. 64 bits at one
// per document outlasts any process.
std::uint64_t NextOwnerSerial() {
  static std::uint64_t next = 0;
  return ++next;
}

bool MatchesAny(const dom::Element& element, const std::vector<css::Selector>& selectors) {
  for (const css::Selector& selector : selectors) {
    if (selector.Matches(element)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool DomBindings::HasUserActivation() const {
  // Through `dom::Document`, where ADR 0028 §1 put the one copy of this bit: the engine's click and key
  // paths write it and nothing else does. A second copy in the binding layer is how two answers about
  // the same gesture come to disagree -- which for a clipboard gate would mean a page writing the
  // clipboard without one.
  return document_ != nullptr && document_->HasUserActivation();
}

DomBindings::DomBindings(js::Interpreter& interpreter, dom::Document& document,
                         std::string url, GeometrySource* geometry, NetworkSource* network,
                         HistorySource* history, StorageSource* storage, CookieSource* cookies,
                         SocketSource* sockets, MediaController* media, CanvasSurface* canvas,
                         WorkerHost* workers, IndexedDbSource* indexed_db,
                         AnimationSource* animations)
    : interpreter_(&interpreter),
      document_(&document),
      url_(std::move(url)),
      geometry_(geometry),
      network_(network),
      history_(history),
      storage_(storage),
      indexed_db_(indexed_db),
      cookies_(cookies),
      sockets_(sockets),
      media_(media),
      canvas_(canvas),
      workers_(workers),
      animations_(animations) {}

js::Value OwnerValue(const DomBindings* owner) {
  return js::Value::Number(owner == nullptr ? 0.0
                                            : static_cast<double>(owner->identity_.Serial()));
}

OwnerIdentity::OwnerIdentity(DomBindings* owner) : serial_(NextOwnerSerial()) {
  LiveOwners().push_back(LiveOwner{serial_, owner});
}

OwnerIdentity::~OwnerIdentity() {
  std::vector<LiveOwner>& live = LiveOwners();
  for (std::size_t i = 0; i < live.size(); ++i) {
    if (live[i].serial == serial_) {
      live[i] = live.back();
      live.pop_back();
      return;
    }
  }
}

std::unique_ptr<dom::Node> DomBindings::TakeUnattached(dom::Node* node) {
  for (std::vector<std::unique_ptr<dom::Node>>* list : {&unattached_, &detached_}) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      if ((*list)[i].get() == node) {
        std::unique_ptr<dom::Node> owned = std::move((*list)[i]);
        list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
        return owned;
      }
    }
  }
  return nullptr;
}

DomBindings* BindingsForDocument(const dom::Document& document) {
  for (const LiveOwner& owner : LiveOwners()) {
    if (owner.bindings != nullptr && &owner.bindings->Document() == &document) {
      return owner.bindings;
    }
  }
  return nullptr;
}

DomBindings* OwnerOf(const js::NativeCall& call) {
  const js::Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  const auto serial = static_cast<std::uint64_t>(slot->number);
  // A linear scan, and it stays one: the list holds one entry per *live*
  // document in this process, which is the top-level page plus its scripted
  // frames -- bounded at `js::kMaxRealms` + 1 and one or two in practice. A
  // hash table over two entries is slower than the compare it replaces, and
  // this runs on the way into every native the binding layer installs.
  for (const LiveOwner& owner : LiveOwners()) {
    if (owner.serial == serial) {
      return owner.bindings;
    }
  }
  return nullptr;
}

bool DomBindings::Matches(const dom::Element& element, const std::string& selector) {
  // The real CSS selector engine, not the three-form toy this used to be.
  // `#id`, `.class` and an exact tag were enough for early tests and wrong for
  // every framework: `ytd-app,ytd-masthead`, `div > span`, `:not(.x)` and
  // `[attr]` all silently matched nothing while `querySelector("ytd-app")`
  // still worked, which is how youtube.com looked like it had an app element
  // and no component tree. Parse once per call; querySelectorAll parses once
  // for the whole walk via MatchesSelectorList.
  if (util::EnvFlagEnabled("MICROBROWSER_SELECTOR_TRACE")) {
    // A hang under a real page with real selectors is usually "one query in a
    // hot loop", and the only way to see which is to count. Off by default.
    static std::uint64_t calls = 0;
    if ((++calls % 1000000ULL) == 0ULL) {
      std::fprintf(stderr, "[selector] matches_calls=%llu last=%s\n",
                   static_cast<unsigned long long>(calls), selector.c_str());
    }
  }
  return MatchesSelectorList(element, css::ParseSelectorList(selector));
}

bool DomBindings::MatchesSelectorList(const dom::Element& element,
                                      const std::vector<css::Selector>& selectors) {
  if (util::EnvFlagEnabled("MICROBROWSER_SELECTOR_TRACE")) {
    // querySelectorAll parses once and calls this per element, so the string
    // form of Matches is not on the hot path -- count here or the trace is
    // silent on the hang it exists to diagnose (TD-0013).
    static std::uint64_t calls = 0;
    if ((++calls % 5'000'000ULL) == 0ULL) {
      std::fprintf(stderr, "[selector] list_match_calls=%llu selectors=%zu\n",
                   static_cast<unsigned long long>(calls), selectors.size());
    }
  }
  return MatchesAny(element, selectors);
}

js::Value DomBindings::WrapperFor(dom::Node* node) {
  if (node == nullptr) {
    return Value::Null();  // `null`, not undefined: an absent node is the DOM's null
  }
  // **A node is wrapped by its node document's binding layer, whoever was
  // asked.** ADR 0042 §5 step 3, and it is the difference between a wrapper
  // cache and a wrapper cache *per realm*. Two layers over one heap -- which is
  // what a same-origin `<iframe>` is -- would otherwise give one node two
  // wrappers, so `parent.document.body === parent.document.body` read from the
  // frame answers false, and a node adopted into the top-level document keeps
  // the frame realm's prototypes, so `instanceof Text` answers false about a
  // Text node.
  //
  // The lookup is over the same live-owner list `OwnerOf` walks, for the same
  // reason it is a list: there is one entry per live document, which is the page
  // plus its scripted frames. The delegate call cannot recurse more than once,
  // because the layer it finds is by definition the one whose document this is.
  if (node->NodeDocument() != nullptr && node->NodeDocument() != document_) {
    if (DomBindings* home = BindingsForDocument(*node->NodeDocument());
        home != nullptr && home != this) {
      return home->WrapperFor(node);
    }
  }
  if (!wrappers_.IsObject()) {
    wrappers_ = interpreter_->NewObjectValue();
    if (!wrappers_.IsObject()) {
      return Value::Null();
    }
    // Hung off the global object, which is unconditionally a GC root.
    //
    // This member alone is *not* enough, and getting that wrong is a
    // use-after-free rather than a missing feature: the collector cannot see a
    // `js::Value` sitting in a C++ field, so the first collection freed every
    // wrapper and left this pointing at reclaimed memory. The test that runs a
    // collection and then compares two wrappers is what found it.
    //
    // The global rather than the `document` wrapper because the global is
    // rooted from the moment the interpreter exists, so there is no window
    // during setup where the cache is unreachable.
    interpreter_->Global()->Set("#domWrappers", wrappers_);
  }
  // Identity: the same node yields the same object every time. Script uses a
  // wrapper as a set key and a map key, and handing out a fresh one per access
  // breaks both quietly.
  const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(node));
  if (const Value* cached = wrappers_.object->GetOwn(key)) {
    // Shadow-root wrappers created before ShadyDOM replaced `window.ShadowRoot`
    // keep the stale native prototype until reconciled here.
    if (node->IsDocumentFragment() &&
        static_cast<const dom::DocumentFragment&>(*node).Host() != nullptr) {
      const Value prototype = PrototypeFor(*node);
      if (prototype.IsObject() && cached->IsObject()) {
        cached->object->SetPrototype(prototype.object);
      }
    }
    return *cached;
  }

  const Value wrapper = interpreter_->NewObjectValue();
  if (!wrapper.IsObject()) {
    return Value::Null();
  }
  wrapper.object->Set(kNodeSlot, PointerValue(node));
  // The interface, rather than a copy of it. Every method and accessor used to
  // be an own property of every wrapper -- which cost one native function per
  // property per node, and made `instanceof` unanswerable because there was no
  // shared object to compare against. See NodeInterfaces.cpp and ADR 0012.
  const Value prototype = PrototypeFor(*node);
  if (prototype.IsObject()) {
    wrapper.object->SetPrototype(prototype.object);
  }
  wrappers_.object->Set(key, wrapper);
  return wrapper;
}

// The methods and accessors every node has, installed on `Node.prototype`.
void DomBindings::InstallNodeInterface(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      // The bindings instance travels on the function object rather than in a
      // capture, because a capture is invisible to the collector -- the same
      // rule every other native in this engine follows.
      native.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  const auto rw_accessor = [this, &target](const char* name, js::NativeFunction get,
                                         js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  // --- Every node ----------------------------------------------------------

  accessor("parentNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    return owner == nullptr || self == nullptr ? Value::Null()
                                               : owner->WrapperFor(self->Parent());
  });
  accessor("childNodes", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> children;
    if (owner != nullptr && self != nullptr) {
      for (const std::unique_ptr<dom::Node>& child : self->Children()) {
        children.push_back(owner->WrapperFor(child.get()));
      }
    }
    return call.interpreter.NewArrayValue(std::move(children));
  });
  // `children` is ParentNode, not Node -- see InstallParentQueries. Putting it
  // on Node.prototype looked equivalent (Element inherits it) until ShadyDOM's
  // noPatch path copied only *own* descriptors off Element.prototype into
  // `__shady_native_children`; a missing own `children` left Polymer.dom's
  // wrapper answering undefined and stampDomArraySplices_ throwing on youtube.
  const auto sibling = [&accessor](const char* name, int step) {
    accessor(name, [step](NativeCall& call) {
      DomBindings* owner = OwnerOf(call);
      dom::Node* self = NodeOf(call.self);
      if (owner == nullptr || self == nullptr || self->Parent() == nullptr) {
        return Value::Null();
      }
      const std::vector<std::unique_ptr<dom::Node>>& siblings = self->Parent()->Children();
      for (std::size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() != self) {
          continue;
        }
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(i) + step;
        if (at < 0 || at >= static_cast<std::ptrdiff_t>(siblings.size())) {
          return Value::Null();
        }
        return owner->WrapperFor(siblings[static_cast<std::size_t>(at)].get());
      }
      return Value::Null();
    });
  };
  sibling("nextSibling", 1);
  sibling("previousSibling", -1);

  accessor("firstChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    return owner == nullptr || self == nullptr ? Value::Null()
                                               : owner->WrapperFor(self->FirstChild());
  });
  accessor("lastChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    return owner == nullptr || self == nullptr ? Value::Null()
                                               : owner->WrapperFor(self->LastChild());
  });
  accessor("nodeName", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return self == nullptr ? Value::Undefined() : Value::String(NodeNameOf(*self));
  });

  accessor("nodeType", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr) {
      return Value::Undefined();
    }
    // The numbers the DOM has always used, which script compares against
    // literals rather than names.
    switch (self->GetKind()) {
      case dom::Node::Kind::Element: return Value::Number(1);
      case dom::Node::Kind::Text:
        return Value::Number(static_cast<const dom::Text*>(self)->IsCData() ? 4 : 3);
      case dom::Node::Kind::Comment: return Value::Number(8);
      case dom::Node::Kind::DocumentFragment: return Value::Number(11);
      case dom::Node::Kind::Document: return Value::Number(9);
      case dom::Node::Kind::DocumentType: return Value::Number(10);
      case dom::Node::Kind::ProcessingInstruction: return Value::Number(7);
    }
    return Value::Number(0);
  });

  // `nodeValue` on Node: the data of a text or comment node, null elsewhere.
  // Polymer and legacy code paths still reach bindings through it.
  rw_accessor(
      "nodeValue",
      [](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        if (self == nullptr) {
          return Value::Undefined();
        }
        return IsCharacterDataNode(*self) ? Value::String(CharacterDataOf(self))
                                         : Value::Null();
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        const std::string value = js::ToString(Argument(call.arguments, 0));
        if (IsCharacterDataNode(*self)) {
          // "Replace data" over the whole node, live ranges and all -- the same
          // act as setting `data`, and the DOM defines it by pointing at that.
          const std::size_t previous = DomStringLength(CharacterDataOf(self));
          owner->SetCharacterData(self, value);
          RangesDidReplaceData(call.interpreter, *self, 0, previous, DomStringLength(value));
        }
        return Value::Undefined();
      });

  // The root of this node's tree. Without it ShadyDOM decides native shadow
  // DOM is incomplete (`attachShadow && getRootNode`) and takes over -- and
  // youtube's polyfill then calls `fragment.za(...)` on *our* ShadowRoot
  // prototype, which has no such method. With it, ShadyDOM stays out and
  // Polymer stamps through the native `attachShadow` + `appendChild` path.
  method("getRootNode", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return Value::Null();
    }
    bool composed = false;
    const Value options = Argument(call.arguments, 0);
    if (options.IsObject()) {
      if (const Value* flag = options.object->Get("composed")) {
        composed = js::ToBoolean(*flag);
      }
    }
    dom::Node* root = self;
    for (int depth = 0; depth < 10000; ++depth) {
      if (dom::Node* parent = root->Parent()) {
        root = parent;
        continue;
      }
      // A shadow root has no parent. `composed: true` climbs out through the
      // host; otherwise the root *is* the answer -- which is what makes
      // `element.getRootNode()` inside a component return the shadow root.
      if (composed) {
        if (const dom::Element* host = dom::ShadowHostOf(*root)) {
          root = const_cast<dom::Element*>(host);
          continue;
        }
      }
      break;
    }
    return owner->WrapperFor(root);
  });

  // The event methods are *not* installed here any more. They live on
  // `EventTarget.prototype`, which Node inherits from -- same methods, same
  // objects, one link further up. That is where the specification puts them and
  // where a polyfill looks for them; see EnsureInterfaces.

  InstallMutationMethods(target);

  // --- Text content --------------------------------------------------------

  method("appendText", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "appendText called on a non-node");
    }
    return owner->AppendTextTo(*self, js::ToString(Argument(call.arguments, 0)));
  });
}

// What an element has beyond a node, installed on `Element.prototype`.
void DomBindings::InstallElementInterface(const js::Value& target) {
  const auto method = [this, &target](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      // The bindings instance travels on the function object rather than in a
      // capture, because a capture is invisible to the collector -- the same
      // rule every other native in this engine follows.
      native.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  const auto rw_accessor = [this, &target](const char* name, js::NativeFunction get,
                                           js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      setter.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->DefineAccessor(name, getter.object, setter.object);
    }
  };

  // The same string `nodeName` gives, because the DOM says they are the same
  // string. See NodeNameOf.
  accessor("tagName", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return Value::String(NodeNameOf(*self));
  });
  // `id` and `className` are not here any more. They were getter-only, so
  // `el.id = 'x'` was a silent no-op -- see ReflectedAttributes.cpp, which
  // installs both halves of every attribute a page writes through a property.
  method("getAttribute", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "getAttribute called on a non-element");
    }
    auto& element = static_cast<dom::Element&>(*self);
    const std::string* value = element.GetAttribute(
        AttributeNameFor(element, js::ToString(Argument(call.arguments, 0))));
    // Null rather than undefined for an absent attribute, which is what
    // `el.getAttribute('x') === null` tests for. Binding tokens like
    // `[[items]]` are real attribute values until Polymer replaces them --
    // hiding them blocked dom-repeat and every other attribute binding.
    return value == nullptr ? Value::Null() : Value::String(*value);
  });
  // The `…NS` half matches on (namespace, local name) rather than on the
  // qualified name, which is a different question and not a stricter one: an
  // element can carry both `foo` in no namespace and `x:foo` in one, and only
  // this half can tell them apart. Nothing is lower-cased here -- the DOM
  // lower-cases a *qualified* name argument and never a local one.
  method("getAttributeNS", [](NativeCall& call) -> Value {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "getAttributeNS called on a non-element");
    }
    dom::NamespaceRef name_space;
    std::string local;
    if (!ToNamespaceAndLocalName(call, Argument(call.arguments, 0),
                                 Argument(call.arguments, 1), name_space, local)) {
      return call.ThrownValue();
    }
    const dom::Attribute* found =
        static_cast<dom::Element*>(self)->GetAttributeNS(name_space, local);
    return found == nullptr ? Value::Null() : Value::String(found->value);
  });
  method("hasAttribute", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Bool(false);
    }
    auto& element = static_cast<dom::Element&>(*self);
    return Value::Bool(element.HasAttribute(
        AttributeNameFor(element, js::ToString(Argument(call.arguments, 0)))));
  });
  method("hasAttributeNS", [](NativeCall& call) -> Value {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Bool(false);
    }
    dom::NamespaceRef name_space;
    std::string local;
    if (!ToNamespaceAndLocalName(call, Argument(call.arguments, 0),
                                 Argument(call.arguments, 1), name_space, local)) {
      return call.ThrownValue();
    }
    return Value::Bool(static_cast<dom::Element*>(self)->GetAttributeNS(name_space, local) !=
                       nullptr);
  });
  // Every qualified name on the element, in order. A page uses it to copy an
  // element's attributes without a NamedNodeMap, and a serializer needs the
  // order to be the insertion one.
  method("getAttributeNames", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> names;
    if (self != nullptr && self->IsElement()) {
      for (const dom::Attribute& attribute : static_cast<dom::Element*>(self)->Attributes()) {
        names.push_back(Value::String(attribute.name));
      }
    }
    return call.interpreter.NewArrayValue(std::move(names));
  });
  method("removeAttribute", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "removeAttribute called on a non-element");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    // One implementation of "an attribute changed", shared with the reflected
    // properties: two would be two chances to forget the custom-element
    // reaction or the mutation record.
    auto& element = static_cast<dom::Element&>(*self);
    owner->RemoveElementAttribute(
        element, AttributeNameFor(element, js::ToString(Argument(call.arguments, 0))));
    return Value::Undefined();
  });
  method("removeAttributeNS", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "removeAttributeNS called on a non-element");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    dom::NamespaceRef name_space;
    std::string local;
    if (!ToNamespaceAndLocalName(call, Argument(call.arguments, 0),
                                 Argument(call.arguments, 1), name_space, local)) {
      return call.ThrownValue();
    }
    owner->RemoveElementAttributeNS(static_cast<dom::Element&>(*self), name_space, local);
    return Value::Undefined();
  });
  // `toggleAttribute(name, force?)`. Returns whether the attribute is present
  // afterwards, which is what makes the one-argument form usable as a flip.
  method("toggleAttribute", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "toggleAttribute called on a non-element");
    }
    if (!RequireArguments(call, "Element", "toggleAttribute", 1)) {
      return call.ThrownValue();
    }
    std::string name;
    if (!ToDomString(call, call.arguments[0], name)) {
      return call.ThrownValue();
    }
    if (!IsValidLocalName(name, NameKind::Attribute)) {
      return ThrowDom(call, "InvalidCharacterError",
                      "'" + name + "' is not a valid attribute name");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    auto& element = static_cast<dom::Element&>(*self);
    const std::string wanted = AttributeNameFor(element, name);
    const bool present = element.HasAttribute(wanted);
    // `force` is a *supplied* boolean rather than a truthy one: the two-argument
    // form pins the outcome, and the one-argument form flips. An absent
    // argument and an explicit `undefined` are the same thing here, which is
    // what WebIDL's optional-without-a-default means.
    const bool forced = call.arguments.size() > 1 && !call.arguments[1].IsUndefined();
    const bool wanted_present = forced ? js::ToBoolean(call.arguments[1]) : !present;
    if (wanted_present && !present) {
      owner->SetElementAttribute(element, wanted, std::string());
    } else if (!wanted_present && present) {
      owner->RemoveElementAttribute(element, wanted);
    }
    return Value::Bool(wanted_present);
  });
  method("matches", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && self->IsElement() &&
                       Matches(static_cast<dom::Element&>(*self),
                               js::ToString(Argument(call.arguments, 0))));
  });
  method("closest", [](NativeCall& call) {
    // This element or the nearest ancestor that matches, which is how a
    // click handler finds the row a button is in.
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return Value::Null();
    }
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    for (dom::Node* walk = self; walk != nullptr; walk = walk->Parent()) {
      if (walk->IsElement() && Matches(static_cast<dom::Element&>(*walk), selector)) {
        return owner->WrapperFor(walk);
      }
    }
    return Value::Null();
  });
  // `classList`, as the `DOMTokenList` the DOM defines. See TokenList.cpp.
  //
  // Cached on the wrapper, which is the one thing about it that is not
  // obvious: the list is *live* -- it reads the `class` attribute on every
  // operation -- so caching it costs nothing in staleness, and not caching it
  // makes `el.classList !== el.classList`. Pages compare them.
  //
  // The setter is WebIDL's `[PutForwards=value]`: `el.classList = "foo"` is
  // legal and assigns the *attribute*, leaving the list object itself in
  // place. Without a setter at all, the same line is a silent no-op in sloppy
  // mode and a TypeError under `"use strict"`.
  rw_accessor(
      "classList",
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr || !self->IsElement()) {
          return Value::Undefined();
        }
        if (call.self.IsObject()) {
          if (const Value* cached = call.self.object->GetOwn(kClassListSlot)) {
            return *cached;
          }
        }
        const Value list = owner->MakeTokenList(static_cast<dom::Element&>(*self), "class");
        if (call.self.IsObject()) {
          call.self.object->SetHidden(kClassListSlot, list);
        }
        return list;
      },
      [](NativeCall& call) -> Value {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr || !self->IsElement()) {
          return Value::Undefined();
        }
        std::string value;
        if (!CoerceToString(call, Argument(call.arguments, 0), value)) {
          return call.ThrownValue();
        }
        owner->SetElementAttribute(static_cast<dom::Element&>(*self), "class", value);
        return Value::Undefined();
      });
  // `el.style.display = 'none'`, backed by the `style` attribute the cascade
  // already reads. A fresh object per read, like `classList`, and for the
  // same reason: the attribute is the state, and a parsed copy would go
  // stale the moment anything else wrote it.
  accessor("style", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return owner->MakeStyle(static_cast<dom::Element&>(*self));
  });
  // `data-*` attributes, under the names a page uses for them. Live, because
  // `el.dataset.version = url` must write `data-version` -- youtube's player
  // does exactly that, and a snapshot Proxy-less object threw the write away so
  // J14's version check always failed and EHT cleared `#movie_player`.
  accessor("dataset", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return owner->MakeDataset(static_cast<dom::Element&>(*self));
  });
  method("setAttribute", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "setAttribute called on a non-element");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    // Both name and value are DOMStrings: coerce via toString, not pure
    // js::ToString (Location/URL → "[object Object]" broke reflected URL attrs
    // and the same bug reaches setAttribute('href'|'src', location)).
    std::string name;
    std::string value;
    if (!CoerceToString(call, Argument(call.arguments, 0), name) ||
        !CoerceToString(call, Argument(call.arguments, 1), value)) {
      return call.ThrownValue();
    }
    // An attribute name with a space or an `=` in it cannot be serialised back
    // into markup, so it is refused rather than stored. The attribute rule is
    // the element one plus `=`, which is what separates a name from its value.
    if (!IsValidLocalName(name, NameKind::Attribute)) {
      return ThrowDom(call, "InvalidCharacterError",
                      "'" + name + "' is not a valid attribute name");
    }
    auto& element = static_cast<dom::Element&>(*self);
    owner->SetElementAttribute(element, AttributeNameFor(element, name), value);
    return Value::Undefined();
  });
  method("setAttributeNS", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "setAttributeNS called on a non-element");
    }
    if (!RequireArguments(call, "Element", "setAttributeNS", 3)) {
      return call.ThrownValue();
    }
    QualifiedName name;
    if (!ToQualifiedName(call, call.arguments[0], call.arguments[1], NameKind::Attribute,
                         name)) {
      return call.ThrownValue();
    }
    std::string value;
    if (!CoerceToString(call, Argument(call.arguments, 2), value)) {
      return call.ThrownValue();
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    owner->SetElementAttributeNS(static_cast<dom::Element&>(*self), name.name_space,
                                 name.qualified, name.prefix_length, value);
    return Value::Undefined();
  });
}

dom::Element* DomBindings::FindElementIn(
    dom::Node& root, const std::function<bool(const dom::Element&)>& matches) {
  dom::Element* found = nullptr;
  // Depth-first, in document order, which is what every selector API promises.
  root.ForEachDescendant([&](const dom::Node& node) {
    if (found != nullptr || !node.IsElement()) {
      return;
    }
    const auto& element = static_cast<const dom::Element&>(node);
    if (matches(element)) {
      found = const_cast<dom::Element*>(&element);
    }
  });
  return found;
}

void DomBindings::ForEachElementIn(dom::Node& root,
                                   const std::function<void(dom::Element&)>& visit) {
  root.ForEachDescendant([&](const dom::Node& node) {
    if (node.IsElement()) {
      visit(const_cast<dom::Element&>(static_cast<const dom::Element&>(node)));
    }
  });
}

void DomBindings::ForEachElement(const std::function<void(dom::Element&)>& visit) const {
  ForEachElementIn(*document_, visit);
}

// Which tree a `document.*` call is about.
//
// The receiver when it is a document, and the page's own otherwise -- which
// covers `document.getElementById(...)` and a method pulled off and called on
// nothing alike. It exists because `document` stopped being the only document
// a page can hold: `document.implementation.createHTMLDocument()` makes a real
// second one, and a query on it that answered about the page on screen would
// be exactly the stub ADR 0012 calls worse than an absence -- a name that
// resolves, a call that succeeds, and an answer about the wrong tree.
//
// Deliberately not "any node": these are document methods, and
// `Document.prototype.getElementById.call(someDiv, 'x')` is a page asking for
// something the interface does not offer.
dom::Document* DomBindings::DocumentOf(const js::Value& self) const {
  dom::Node* receiver = NodeOf(self);
  if (receiver != nullptr && receiver->GetKind() == dom::Node::Kind::Document) {
    return static_cast<dom::Document*>(receiver);
  }
  return document_;
}

void DomBindings::InstallImageElement(const js::Value& target) {
  if (!target.IsObject() || geometry_ == nullptr) {
    return;
  }
  const auto install = [this, &target](const char* name, auto read) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(read));
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      target.object->DefineAccessor(name, getter.object, nullptr);
    }
  };
  install("naturalWidth", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || owner->geometry_ == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Number(0.0);
    }
    const std::optional<ImageState> state =
        owner->geometry_->QueryImageState(static_cast<const dom::Element&>(*self));
    return Value::Number(state.has_value() ? static_cast<double>(state->natural_width) : 0.0);
  });
  install("naturalHeight", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || owner->geometry_ == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Number(0.0);
    }
    const std::optional<ImageState> state =
        owner->geometry_->QueryImageState(static_cast<const dom::Element&>(*self));
    return Value::Number(state.has_value() ? static_cast<double>(state->natural_height) : 0.0);
  });
  install("complete", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || owner->geometry_ == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Bool(false);
    }
    const std::optional<ImageState> state =
        owner->geometry_->QueryImageState(static_cast<const dom::Element&>(*self));
    return Value::Bool(state.has_value() && state->complete);
  });
}

}  // namespace microbrowser::bindings
