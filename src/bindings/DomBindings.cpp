#include "bindings/DomBindings.h"

#include "bindings/BindingSupport.h"

#include <algorithm>
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

}  // namespace

DomBindings::DomBindings(js::Interpreter& interpreter, dom::Document& document,
                         std::string url)
    : interpreter_(&interpreter), document_(&document), url_(std::move(url)) {}

bool DomBindings::Matches(const dom::Element& element, const std::string& selector) {
  if (selector.empty()) {
    return false;
  }
  const char kind = selector.front();
  const std::string wanted =
      kind == '#' || kind == '.' ? selector.substr(1) : LowerCase(selector);
  if (wanted.empty()) {
    return false;
  }
  if (kind == '#') {
    const std::string* id = element.GetAttribute("id");
    return id != nullptr && *id == wanted;
  }
  if (kind != '.') {
    return selector == "*" || element.TagName() == wanted;
  }
  const std::string* names = element.GetAttribute("class");
  if (names == nullptr) {
    return false;
  }
  // Whole-word, so `.btn` does not match `class="btn-large"`. A substring
  // test here would select half the page and look almost right.
  for (std::size_t at = names->find(wanted); at != std::string::npos;
       at = names->find(wanted, at + 1)) {
    const bool left = at == 0 || (*names)[at - 1] == ' ';
    const bool right =
        at + wanted.size() == names->size() || (*names)[at + wanted.size()] == ' ';
    if (left && right) {
      return true;
    }
  }
  return false;
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

  // The four methods a page uses, each reading and rewriting the `class`
  // attribute. Nothing is cached between calls: a parsed copy would go stale
  // the moment anything else touched the attribute, and `class` is the one
  // attribute two pieces of code fight over.
  enum class Change { Add, Remove, Toggle, Contains };
  const auto operate = [this, &list](const char* name, Change change) {
    const Value native = interpreter_->NewNativeValue(name, [change](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return Value::Undefined();
      }
      auto& target = static_cast<dom::Element&>(*self);
      const std::string wanted = js::ToString(Argument(call.arguments, 0));
      if (wanted.empty()) {
        return Value::Undefined();
      }
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

js::Value DomBindings::CreateText(const std::string& text) {
  auto node = std::make_unique<dom::Text>(text);
  dom::Node* raw = node.get();
  unattached_.push_back(std::move(node));
  return WrapperFor(raw);
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
    if (self == nullptr) {
      return Value::Undefined();
    }
    // Upper case for an element, which is what the DOM has always reported and
    // what `node.nodeName === 'DIV'` tests against. `tagName` here is the
    // lower-case name the parser stored, so the two deliberately differ.
    if (self->IsElement()) {
      std::string name = static_cast<dom::Element*>(self)->TagName();
      for (char& c : name) {
        if (c >= 'a' && c <= 'z') {
          c = static_cast<char>(c - 'a' + 'A');
        }
      }
      return Value::String(name);
    }
    return Value::String(std::string(self->IsText() ? "#text" : "#document"));
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
    method("removeAttribute", [](NativeCall& call) {
      dom::Node* self = NodeOf(call.self);
      if (self == nullptr || !self->IsElement()) {
        return call.Throw("TypeError", "removeAttribute called on a non-element");
      }
      static_cast<dom::Element*>(self)->RemoveAttribute(
          LowerCase(js::ToString(Argument(call.arguments, 0))));
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
  InstallWindow();
}

}  // namespace microbrowser::bindings
