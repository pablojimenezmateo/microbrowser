#include "bindings/DomBindings.h"

#include "bindings/BindingSupport.h"
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

js::Value DomBindings::MakeClassList(dom::Element& element) {
  const Value list = interpreter_->NewObjectValue();
  if (!list.IsObject()) {
    return list;
  }
  // The element travels on the object rather than in a capture, the same rule
  // every native in this engine follows: a capture is invisible to the
  // collector, and a raw pointer in one is a lifetime nobody is tracking.
  list.object->Set(kNodeSlot, PointerValue(&element));

  const auto class_names = [](const dom::Element& target) {
    const std::string* current = target.GetAttribute("class");
    std::vector<std::string> names;
    std::string word;
    for (const char c : current == nullptr ? std::string() : *current) {
      if (c == ' ' || c == '\t' || c == '\n') {
        if (!word.empty()) {
          names.push_back(word);
          word.clear();
        }
        continue;
      }
      word.push_back(c);
    }
    if (!word.empty()) {
      names.push_back(word);
    }
    return names;
  };

  // length + item + Symbol.iterator: DOMTokenList is iterable. youtube builds
  // a CSS-path diagnostic with `for (const c of _.A(el.classList))`, and our
  // four-method object was truthy but neither iterable nor length-bearing --
  // Closure's `_.A` then threw `Error: c\`[object Object]` and aborted the
  // custom-element reaction mid-stamp.
  const Value length = interpreter_->NewNativeValue("length", [class_names](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Number(0);
    }
    return Value::Number(
        static_cast<double>(class_names(*static_cast<dom::Element*>(self)).size()));
  });
  if (length.IsObject()) {
    length.object->Set(kOwnerSlot, PointerValue(this));
    list.object->DefineAccessor("length", length.object, nullptr);
  }
  const Value item = interpreter_->NewNativeValue("item", [class_names](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return Value::Null();
    }
    const auto names = class_names(*static_cast<dom::Element*>(self));
    const double index = js::ToNumber(Argument(call.arguments, 0));
    if (!(index >= 0) || index >= static_cast<double>(names.size())) {
      return Value::Null();
    }
    return Value::String(names[static_cast<std::size_t>(index)]);
  });
  if (item.IsObject()) {
    item.object->Set(kOwnerSlot, PointerValue(this));
    list.object->Set("item", item);
  }
  const Value iterate =
      interpreter_->NewNativeValue("[Symbol.iterator]", [class_names](NativeCall& call) {
        dom::Node* self = NodeOf(call.self);
        std::vector<Value> out;
        if (self != nullptr && self->IsElement()) {
          for (const std::string& name : class_names(*static_cast<dom::Element*>(self))) {
            out.push_back(Value::String(name));
          }
        }
        const Value entries = call.interpreter.NewArrayValue(std::move(out));
        if (!entries.IsObject()) {
          return Value::Undefined();
        }
        const js::Value* protocol = entries.object->Get(
            js::PropertyKey::Symbol(call.interpreter.SymbolIterator()));
        if (protocol == nullptr) {
          return Value::Undefined();
        }
        const js::Result made = call.interpreter.CallFunction(*protocol, entries, {});
        return made.completion == js::Completion::Throw ? Value::Undefined() : made.value;
      });
  if (iterate.IsObject()) {
    iterate.object->Set(kOwnerSlot, PointerValue(this));
    list.object->Set(js::PropertyKey::Symbol(interpreter_->SymbolIterator()), iterate);
  }

  // The four methods a page uses, each reading and rewriting the `class`
  // attribute. Nothing is cached between calls: a parsed copy would go stale
  // the moment anything else touched the attribute, and `class` is the one
  // attribute two pieces of code fight over.
  enum class Change { Add, Remove, Toggle, Contains };
  const auto operate = [this, &list, class_names](const char* name, Change change) {
    const Value native = interpreter_->NewNativeValue(name, [change, class_names](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return Value::Undefined();
      }
      auto& target = static_cast<dom::Element&>(*self);
      const std::string wanted = js::ToString(Argument(call.arguments, 0));
      if (wanted.empty()) {
        return Value::Undefined();
      }
      std::vector<std::string> names = class_names(target);

      const auto found = std::find(names.begin(), names.end(), wanted);
      const bool present = found != names.end();
      if (change == Change::Contains) {
        return Value::Bool(present);
      }
      const bool wants = change == Change::Add || (change == Change::Toggle && !present);
      if (wants && !present) {
        names.push_back(wanted);
      } else if (!wants && present) {
        names.erase(found);
      }
      std::string rewritten;
      for (const std::string& each : names) {
        if (!rewritten.empty()) {
          rewritten.push_back(' ');
        }
        rewritten += each;
      }
      target.SetAttribute("class", rewritten);
      return change == Change::Toggle ? Value::Bool(wants) : Value::Undefined();
    });
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      list.object->Set(name, native);
    }
  };
  operate("add", Change::Add);
  operate("remove", Change::Remove);
  operate("toggle", Change::Toggle);
  operate("contains", Change::Contains);
  return list;
}


js::Value DomBindings::WrapperFor(dom::Node* node) {
  if (node == nullptr) {
    return Value::Null();  // `null`, not undefined: an absent node is the DOM's null
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
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
    }
  };
  const auto rw_accessor = [this, &target](const char* name, js::NativeFunction get,
                                         js::NativeFunction set) {
    const Value getter = interpreter_->NewNativeValue(name, std::move(get));
    const Value setter = interpreter_->NewNativeValue(name, std::move(set));
    if (getter.IsObject() && setter.IsObject()) {
      getter.object->Set(kOwnerSlot, PointerValue(this));
      setter.object->Set(kOwnerSlot, PointerValue(this));
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
      case dom::Node::Kind::Text: return Value::Number(3);
      case dom::Node::Kind::Comment: return Value::Number(8);
      case dom::Node::Kind::DocumentFragment: return Value::Number(11);
      case dom::Node::Kind::Document: return Value::Number(9);
      case dom::Node::Kind::DocumentType: return Value::Number(10);
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
        if (self->IsText()) {
          return Value::String(static_cast<dom::Text*>(self)->Data());
        }
        if (self->GetKind() == dom::Node::Kind::Comment) {
          return Value::String(static_cast<dom::Comment*>(self)->Data());
        }
        return Value::Null();
      },
      [](NativeCall& call) {
        DomBindings* owner = OwnerOf(call);
        dom::Node* self = NodeOf(call.self);
        if (owner == nullptr || self == nullptr) {
          return Value::Undefined();
        }
        const std::string value = js::ToString(Argument(call.arguments, 0));
        if (self->IsText() || self->GetKind() == dom::Node::Kind::Comment) {
          owner->SetCharacterData(self, value);
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
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->Set(name, native);
    }
  };
  const auto accessor = [this, &target](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      target.object->DefineAccessor(name, native.object, nullptr);
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
    const std::string* value = static_cast<dom::Element*>(self)->GetAttribute(
        LowerCase(js::ToString(Argument(call.arguments, 0))));
    // Null rather than undefined for an absent attribute, which is what
    // `el.getAttribute('x') === null` tests for. Binding tokens like
    // `[[items]]` are real attribute values until Polymer replaces them --
    // hiding them blocked dom-repeat and every other attribute binding.
    return value == nullptr ? Value::Null() : Value::String(*value);
  });
  // Namespace accepted and ignored, for the same reason as createElementNS:
  // this tree is HTML-only. youtube's player calls setAttributeNS(null, ...)
  // during bootstrap; without the name the call throws and #container stays
  // empty.
  method("getAttributeNS", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "getAttributeNS called on a non-element");
    }
    const std::string* value = static_cast<dom::Element*>(self)->GetAttribute(
        LowerCase(js::ToString(Argument(call.arguments, 1))));
    return value == nullptr ? Value::Null() : Value::String(*value);
  });
  method("hasAttribute", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && self->IsElement() &&
                       static_cast<dom::Element*>(self)->HasAttribute(
                           LowerCase(js::ToString(Argument(call.arguments, 0)))));
  });
  method("hasAttributeNS", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return Value::Bool(self != nullptr && self->IsElement() &&
                       static_cast<dom::Element*>(self)->HasAttribute(
                           LowerCase(js::ToString(Argument(call.arguments, 1)))));
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
    owner->RemoveElementAttribute(*static_cast<dom::Element*>(self),
                                  LowerCase(js::ToString(Argument(call.arguments, 0))));
    return Value::Undefined();
  });
  method("removeAttributeNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "removeAttributeNS called on a non-element");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    owner->RemoveElementAttribute(*static_cast<dom::Element*>(self),
                                  LowerCase(js::ToString(Argument(call.arguments, 1))));
    return Value::Undefined();
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
  // `classList`, as a fresh object per read holding the four methods a page
  // uses. Not cached, because it reads and writes the `class` attribute on
  // every call rather than holding a parsed copy that could go stale.
  accessor("classList", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr || !self->IsElement()) {
      return Value::Undefined();
    }
    return owner->MakeClassList(static_cast<dom::Element&>(*self));
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
    owner->SetElementAttribute(*static_cast<dom::Element*>(self),
                               LowerCase(js::ToString(Argument(call.arguments, 0))),
                               js::ToString(Argument(call.arguments, 1)));
    return Value::Undefined();
  });
  method("setAttributeNS", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (self == nullptr || !self->IsElement()) {
      return call.Throw("TypeError", "setAttributeNS called on a non-element");
    }
    if (owner == nullptr) {
      return Value::Undefined();
    }
    // Namespace ignored; see getAttributeNS above.
    owner->SetElementAttribute(*static_cast<dom::Element*>(self),
                               LowerCase(js::ToString(Argument(call.arguments, 1))),
                               js::ToString(Argument(call.arguments, 2)));
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
dom::Node* DomBindings::DocumentOf(const js::Value& self) const {
  dom::Node* receiver = NodeOf(self);
  if (receiver != nullptr && receiver->GetKind() == dom::Node::Kind::Document) {
    return receiver;
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
      getter.object->Set(kOwnerSlot, PointerValue(this));
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
