#include "bindings/DomBindings.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microbrowser::bindings {

namespace {

using js::NativeCall;
using js::Object;
using js::Value;

// Where a wrapper keeps the node it stands for, and where the cache keeps its
// entries. A `#` name, the same convention private class fields and the
// engine's other internal slots already use: not real privacy, and the right
// amount until something observes the difference.
constexpr const char* kNodeSlot = "#node";
constexpr const char* kOwnerSlot = "#bindings";

// The node behind a wrapper, or null for anything that is not one.
//
// Every binding starts here rather than trusting its receiver, because a page
// can call one on anything: `Element.prototype.appendChild.call(7, x)` is legal
// JavaScript and must be a TypeError rather than a jump through a bad pointer.
dom::Node* NodeOf(const Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  const Value* slot = value.object->GetOwn(kNodeSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  // The pointer travels as a double, which holds a 53-bit integer exactly --
  // more than any address on a 64-bit machine with a canonical-form pointer.
  // A page can write to `#node`, which is why this is checked against the
  // cache below rather than dereferenced on the strength of the number alone.
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(slot->number));
}

DomBindings* OwnerOf(const NativeCall& call) {
  const Value* slot = call.callee == nullptr ? nullptr : call.callee->GetOwn(kOwnerSlot);
  if (slot == nullptr || !slot->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<DomBindings*>(static_cast<std::uintptr_t>(slot->number));
}

// An argument, or undefined when the caller passed fewer.
//
// A local copy rather than `js::Argument`, which lives in a header `src/js`
// keeps to itself. That is the module boundary working: a binding is a
// consumer of the engine's *public* surface, and reaching past it for a
// three-line helper would be the first crack in the line this module's
// manifest calls a security boundary.
Value Argument(const std::vector<Value>& arguments, std::size_t index) {
  return index < arguments.size() ? arguments[index] : Value::Undefined();
}

Value PointerValue(const void* pointer) {
  return Value::Number(static_cast<double>(reinterpret_cast<std::uintptr_t>(pointer)));
}

std::string LowerCase(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

}  // namespace

DomBindings::DomBindings(js::Interpreter& interpreter, dom::Document& document)
    : interpreter_(&interpreter), document_(&document) {}

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
    return *cached;
  }

  const Value wrapper = interpreter_->NewObjectValue();
  if (!wrapper.IsObject()) {
    return Value::Null();
  }
  wrapper.object->Set(kNodeSlot, PointerValue(node));
  wrappers_.object->Set(key, wrapper);

  const auto method = [this, &wrapper](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      // The bindings instance travels on the function object rather than in a
      // capture, because a capture is invisible to the collector -- the same
      // rule every other native in this engine follows.
      native.object->Set(kOwnerSlot, PointerValue(this));
      wrapper.object->Set(name, native);
    }
  };
  const auto accessor = [this, &wrapper](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      wrapper.object->DefineAccessor(name, native.object, nullptr);
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
  accessor("children", [](NativeCall& call) {
    // Elements only, unlike `childNodes` -- the distinction that trips up
    // anyone who indexes into the wrong one and gets a whitespace text node.
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    std::vector<Value> children;
    if (owner != nullptr && self != nullptr) {
      for (const std::unique_ptr<dom::Node>& child : self->Children()) {
        if (child->IsElement()) {
          children.push_back(owner->WrapperFor(child.get()));
        }
      }
    }
    return call.interpreter.NewArrayValue(std::move(children));
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
      case dom::Node::Kind::Document: return Value::Number(9);
      case dom::Node::Kind::DocumentType: return Value::Number(10);
    }
    return Value::Number(0);
  });

  method("appendChild", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    dom::Node* child = NodeOf(Argument(call.arguments, 0));
    if (owner == nullptr || self == nullptr || child == nullptr) {
      return call.Throw("TypeError", "appendChild requires a node");
    }
    // Only a node this layer made and has not yet placed can be appended.
    // Moving an existing one means detaching it from its parent, and detaching
    // is what `Node::Remove` does -- the operation this slice deliberately
    // does not have a caller for. See the note on the class.
    if (child->Parent() != nullptr) {
      return call.Throw("TypeError", "moving a node that already has a parent is not supported");
    }
    return owner->AdoptInto(*self, child);
  });

  // --- Elements ------------------------------------------------------------

  if (node->IsElement()) {
    accessor("tagName", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return Value::Undefined();
      }
      return Value::String(static_cast<dom::Element*>(self)->TagName());
    });
    accessor("id", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      const std::string* id = self != nullptr && self->IsElement()
                                  ? static_cast<dom::Element*>(self)->GetAttribute("id")
                                  : nullptr;
      return Value::String(id == nullptr ? std::string() : *id);
    });
    accessor("className", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      const std::string* name = self != nullptr && self->IsElement()
                                    ? static_cast<dom::Element*>(self)->GetAttribute("class")
                                    : nullptr;
      return Value::String(name == nullptr ? std::string() : *name);
    });
    method("getAttribute", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return call.Throw("TypeError", "getAttribute called on a non-element");
      }
      const std::string* value = static_cast<dom::Element*>(self)->GetAttribute(
          LowerCase(js::ToString(Argument(call.arguments, 0))));
      // Null rather than undefined for an absent attribute, which is what
      // `el.getAttribute('x') === null` tests for.
      return value == nullptr ? Value::Null() : Value::String(*value);
    });
    method("hasAttribute", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      return Value::Bool(self != nullptr && self->IsElement() &&
                         static_cast<dom::Element*>(self)->HasAttribute(
                             LowerCase(js::ToString(Argument(call.arguments, 0)))));
    });
    method("setAttribute", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return call.Throw("TypeError", "setAttribute called on a non-element");
      }
      static_cast<dom::Element*>(self)->SetAttribute(
          LowerCase(js::ToString(Argument(call.arguments, 0))),
          js::ToString(Argument(call.arguments, 1)));
      return Value::Undefined();
    });
  }

  // --- Text content --------------------------------------------------------

  method("appendText", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    dom::Node* self = NodeOf(call.self);
    if (owner == nullptr || self == nullptr) {
      return call.Throw("TypeError", "appendText called on a non-node");
    }
    return owner->AppendTextTo(*self, js::ToString(Argument(call.arguments, 0)));
  });
  accessor("textContent", [](NativeCall& call) {
    dom::Node* self = NodeOf(call.self);
    return self == nullptr ? Value::Undefined() : Value::String(self->TextContent());
  });

  return wrapper;
}

dom::Element* DomBindings::FindElement(
    const std::function<bool(const dom::Element&)>& matches) const {
  dom::Element* found = nullptr;
  // Depth-first, in document order, which is what every selector API promises.
  document_->ForEachDescendant([&](const dom::Node& node) {
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

void DomBindings::ForEachElement(const std::function<void(dom::Element&)>& visit) const {
  document_->ForEachDescendant([&](const dom::Node& node) {
    if (node.IsElement()) {
      visit(const_cast<dom::Element&>(static_cast<const dom::Element&>(node)));
    }
  });
}

js::Value DomBindings::CreateElement(const std::string& tag_name) {
  if (tag_name.empty()) {
    return Value::Null();
  }
  auto element = std::make_unique<dom::Element>(tag_name);
  dom::Element* raw = element.get();
  // Held here rather than handed to script, because a node's owner is its
  // parent and this one has none yet. Script gets the wrapper; the node stays
  // owned by C++ until something appends it.
  unattached_.push_back(std::move(element));
  return WrapperFor(raw);
}

js::Value DomBindings::AdoptInto(dom::Node& parent, dom::Node* child) {
  for (std::size_t i = 0; i < unattached_.size(); ++i) {
    if (unattached_[i].get() != child) {
      continue;
    }
    std::unique_ptr<dom::Node> owned = std::move(unattached_[i]);
    unattached_.erase(unattached_.begin() + static_cast<std::ptrdiff_t>(i));
    parent.Append(std::move(owned));
    return WrapperFor(child);
  }
  // Not one of ours to give away. Appending it would mean taking it from its
  // current owner, which is the detach this slice deliberately cannot do.
  return Value::Null();
}

js::Value DomBindings::AppendTextTo(dom::Node& parent, const std::string& text) {
  auto node = std::make_unique<dom::Text>(text);
  dom::Node* raw = node.get();
  parent.Append(std::move(node));
  return WrapperFor(raw);
}

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
    // and this module is not allowed to see it -- widening `allow:` to reach
    // it would widen a security boundary for a convenience, so the selector
    // support that belongs here is the subset that needs no parser.
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    const std::string selector = js::ToString(Argument(call.arguments, 0));
    if (selector.empty()) {
      return Value::Null();
    }
    const char kind = selector.front();
    const std::string wanted =
        kind == '#' || kind == '.' ? selector.substr(1) : LowerCase(selector);
    return owner->WrapperFor(owner->FindElement([&](const dom::Element& element) {
      if (kind == '#') {
        const std::string* id = element.GetAttribute("id");
        return id != nullptr && *id == wanted;
      }
      if (kind == '.') {
        const std::string* names = element.GetAttribute("class");
        if (names == nullptr) {
          return false;
        }
        // Whole-word, so `.btn` does not match `class="btn-large"`.
        std::size_t at = names->find(wanted);
        while (at != std::string::npos) {
          const bool left = at == 0 || (*names)[at - 1] == ' ';
          const bool right =
              at + wanted.size() == names->size() || (*names)[at + wanted.size()] == ' ';
          if (left && right) {
            return true;
          }
          at = names->find(wanted, at + 1);
        }
        return false;
      }
      return element.TagName() == wanted;
    }));
  });
  method("createElement", [](NativeCall& call) {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Null();
    }
    return owner->CreateElement(LowerCase(js::ToString(Argument(call.arguments, 0))));
  });

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

  interpreter_->GlobalScope()->Declare("document", document, false);
}

}  // namespace microbrowser::bindings
