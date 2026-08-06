// `Range`: two boundary points in one tree, and the comparisons between them.
//
// youtube's kevlar bundle stops on `Range` twice over. Once as a bare name in
// a TypeScript decorator's `design:paramtypes` metadata, which needs nothing
// but the name; and once for real, in Closure's `goog.dom.Range`, which is how
// its editor and selection code asks "is this node before that one" --
// `createRange`, `selectNode`, `collapse`, `compareBoundaryPoints` and the four
// `START_TO_*` constants.
//
// **A boundary point is a (node, offset) pair and comparing two of them is the
// whole of this file.** Everything a page reads off a Range -- `collapsed`,
// `commonAncestorContainer`, which of two ranges comes first -- falls out of
// one ordering function over those pairs, so there is one of them here and
// nothing else re-derives it.
//
// What is deliberately **absent**: `extractContents`, `deleteContents`,
// `cloneContents`, `insertNode` and `surroundContents`. Those are tree surgery
// -- the specification's algorithms split text nodes at an offset and reparent
// partially-contained subtrees -- and a version that got the common case right
// and the partial-containment case wrong would corrupt a page's DOM silently.
// ADR 0012's rule points the same way it always does: a page that finds no
// `extractContents` fails where it wrote it, and one that finds a broken one
// fails somewhere else entirely. They are the obvious next piece of this file.
//
// A Range here also does **not** track the tree. The specification keeps
// boundary points live under mutation -- inserting before the start bumps the
// offset -- which needs a registry of live ranges the mutation primitives
// consult. That is real and it is not here; what is here answers correctly
// about the tree as it stands when it is asked, which is how every use in the
// bundle above reads it.

#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

constexpr const char* kStartNodeSlot = "#rangeStartNode";
constexpr const char* kStartOffsetSlot = "#rangeStartOffset";
constexpr const char* kEndNodeSlot = "#rangeEndNode";
constexpr const char* kEndOffsetSlot = "#rangeEndOffset";

dom::Node* NodeSlot(const Value& range, const char* slot) {
  if (!range.IsObject()) {
    return nullptr;
  }
  const Value* found = range.object->GetOwn(slot);
  if (found == nullptr || !found->IsNumber()) {
    return nullptr;
  }
  return reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(found->number));
}

std::uint32_t OffsetSlot(const Value& range, const char* slot) {
  if (!range.IsObject()) {
    return 0;
  }
  const Value* found = range.object->GetOwn(slot);
  return found == nullptr || !found->IsNumber() ? 0
                                                : static_cast<std::uint32_t>(found->number);
}

// How many positions there are inside `node`: its character count for a text
// node or a comment, and its child count for anything else. The specification
// calls this the node's length, and it is what bounds an offset.
std::size_t LengthOf(const dom::Node& node) {
  switch (node.GetKind()) {
    case dom::Node::Kind::Text:
      return static_cast<const dom::Text&>(node).Data().size();
    case dom::Node::Kind::Comment:
      return static_cast<const dom::Comment&>(node).Data().size();
    default:
      return node.Children().size();
  }
}

// Where `node` sits among its parent's children.
std::size_t IndexIn(const dom::Node& node) {
  const dom::Node* parent = node.Parent();
  if (parent == nullptr) {
    return 0;
  }
  const std::vector<std::unique_ptr<dom::Node>>& children = parent->Children();
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (children[i].get() == &node) {
      return i;
    }
  }
  return 0;
}

// The chain from `node` up to its root, root first. Bounded because a tree
// built by script is a tree built by script.
std::vector<const dom::Node*> AncestorsOf(const dom::Node& node) {
  std::vector<const dom::Node*> chain;
  constexpr int kMaxDepth = 100'000;
  for (const dom::Node* at = &node; at != nullptr && chain.size() < kMaxDepth;
       at = at->Parent()) {
    chain.push_back(at);
  }
  // Root first, which is what makes the common-prefix walk below a loop with
  // one index rather than two.
  std::reverse(chain.begin(), chain.end());
  return chain;
}

// **The one ordering function.** -1, 0 or 1 for (a, a_offset) against
// (b, b_offset) in tree order, and 2 for two points in different trees, which
// the specification makes a WrongDocumentError.
//
// Everything else in this file is written in terms of this: `collapsed` is
// "equal", `compareBoundaryPoints` is this directly, and the whole reason it is
// one function is that a second implementation of tree order would eventually
// disagree with this one about a case nobody tested.
int ComparePoints(const dom::Node& a, std::size_t a_offset, const dom::Node& b,
                  std::size_t b_offset) {
  if (&a == &b) {
    if (a_offset == b_offset) {
      return 0;
    }
    return a_offset < b_offset ? -1 : 1;
  }
  const std::vector<const dom::Node*> a_chain = AncestorsOf(a);
  const std::vector<const dom::Node*> b_chain = AncestorsOf(b);
  if (a_chain.empty() || b_chain.empty() || a_chain.front() != b_chain.front()) {
    return 2;  // different trees
  }
  // Walk down the common prefix. The first place they differ decides, and the
  // comparison there is between two *children of the same parent*, which is an
  // index comparison.
  std::size_t depth = 0;
  while (depth < a_chain.size() && depth < b_chain.size() &&
         a_chain[depth] == b_chain[depth]) {
    ++depth;
  }
  if (depth >= a_chain.size()) {
    // `a` is an ancestor of `b`: the point in `a` is before the point in `b`
    // exactly when a_offset is at or before the child that leads down to `b`.
    const std::size_t child = IndexIn(*b_chain[depth]);
    return a_offset <= child ? -1 : 1;
  }
  if (depth >= b_chain.size()) {
    const std::size_t child = IndexIn(*a_chain[depth]);
    return b_offset <= child ? 1 : -1;
  }
  const std::size_t a_index = IndexIn(*a_chain[depth]);
  const std::size_t b_index = IndexIn(*b_chain[depth]);
  return a_index < b_index ? -1 : 1;
}

// The text a range covers, which is `range.toString()` and is how a page reads
// a selection back. Only text nodes contribute, which is the specification's
// rule and is why an element's attributes never appear.
void CollectText(const dom::Node& node, const dom::Node& start, std::size_t start_offset,
                 const dom::Node& end, std::size_t end_offset, std::string& out) {
  if (node.GetKind() == dom::Node::Kind::Text) {
    const std::string& data = static_cast<const dom::Text&>(node).Data();
    const std::size_t from = &node == &start ? std::min(start_offset, data.size()) : 0;
    const std::size_t to = &node == &end ? std::min(end_offset, data.size()) : data.size();
    if (from < to) {
      out.append(data, from, to - from);
    }
    return;
  }
  for (const std::unique_ptr<dom::Node>& child : node.Children()) {
    if (child == nullptr) {
      continue;
    }
    // Only what lies between the two points. `ComparePoints` decides it, so a
    // partially covered subtree is descended into and a fully outside one is
    // skipped whole.
    const std::size_t index = IndexIn(*child);
    if (ComparePoints(node, index + 1, start, start_offset) <= 0) {
      continue;  // entirely before the start
    }
    if (ComparePoints(node, index, end, end_offset) >= 0) {
      break;  // this child and every later one is past the end
    }
    CollectText(*child, start, start_offset, end, end_offset, out);
  }
}

}  // namespace

void DomBindings::InstallRange() {
  EnsureInterfaces();
  if (!interfaces_.IsObject()) {
    return;
  }
  const Value range_interface = MakeInterface("Range", Value::Undefined());
  if (!range_interface.IsObject()) {
    return;
  }

  const auto method = [this, &range_interface](const char* name, js::NativeFunction function) {
    const Value native = interpreter_->NewNativeValue(name, std::move(function));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      range_interface.object->Set(name, native);
    }
  };
  const auto accessor = [this, &range_interface](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, PointerValue(this));
      range_interface.object->DefineAccessor(name, native.object, nullptr);
    }
  };

  // The four constants, on the prototype *and* on the constructor. A page
  // writes `Range.START_TO_END` and Closure writes `goog.dom.Range` against
  // whichever it finds; putting them in one place would work until it did not.
  struct Constant {
    const char* name;
    double value;
  };
  static constexpr Constant kConstants[] = {{"START_TO_START", 0},
                                            {"START_TO_END", 1},
                                            {"END_TO_END", 2},
                                            {"END_TO_START", 3}};
  for (const Constant& constant : kConstants) {
    range_interface.object->Set(constant.name, Value::Number(constant.value));
  }
  if (const Value* constructor = interpreter_->Global()->GetOwn("Range");
      constructor != nullptr && constructor->IsObject()) {
    for (const Constant& constant : kConstants) {
      constructor->object->Set(constant.name, Value::Number(constant.value));
    }
  }

  const auto set_point = [](NativeCall& call, bool start, dom::Node* node, std::size_t offset) {
    if (!call.self.IsObject() || node == nullptr) {
      return;
    }
    call.self.object->SetHidden(start ? kStartNodeSlot : kEndNodeSlot, PointerValue(node));
    call.self.object->SetHidden(start ? kStartOffsetSlot : kEndOffsetSlot,
                                Value::Number(static_cast<double>(offset)));
    // A start after the end collapses the range onto the point just set, which
    // is what the specification says and is what keeps `collapsed` honest
    // without anyone checking the order at read time.
    dom::Node* other = NodeSlot(call.self, start ? kEndNodeSlot : kStartNodeSlot);
    if (other == nullptr) {
      call.self.object->SetHidden(start ? kEndNodeSlot : kStartNodeSlot, PointerValue(node));
      call.self.object->SetHidden(start ? kEndOffsetSlot : kStartOffsetSlot,
                                  Value::Number(static_cast<double>(offset)));
      return;
    }
    const std::size_t other_offset =
        OffsetSlot(call.self, start ? kEndOffsetSlot : kStartOffsetSlot);
    const int order = start ? ComparePoints(*node, offset, *other, other_offset)
                            : ComparePoints(*other, other_offset, *node, offset);
    if (order > 0 || order == 2) {
      call.self.object->SetHidden(start ? kEndNodeSlot : kStartNodeSlot, PointerValue(node));
      call.self.object->SetHidden(start ? kEndOffsetSlot : kStartOffsetSlot,
                                  Value::Number(static_cast<double>(offset)));
    }
  };

  method("setStart", [set_point](NativeCall& call) {
    set_point(call, true, NodeOf(Argument(call.arguments, 0)),
              static_cast<std::size_t>(js::ToNumber(Argument(call.arguments, 1))));
    return Value::Undefined();
  });
  method("setEnd", [set_point](NativeCall& call) {
    set_point(call, false, NodeOf(Argument(call.arguments, 0)),
              static_cast<std::size_t>(js::ToNumber(Argument(call.arguments, 1))));
    return Value::Undefined();
  });
  method("setStartBefore", [set_point](NativeCall& call) {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node != nullptr && node->Parent() != nullptr) {
      set_point(call, true, node->Parent(), IndexIn(*node));
    }
    return Value::Undefined();
  });
  method("setStartAfter", [set_point](NativeCall& call) {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node != nullptr && node->Parent() != nullptr) {
      set_point(call, true, node->Parent(), IndexIn(*node) + 1);
    }
    return Value::Undefined();
  });
  method("setEndBefore", [set_point](NativeCall& call) {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node != nullptr && node->Parent() != nullptr) {
      set_point(call, false, node->Parent(), IndexIn(*node));
    }
    return Value::Undefined();
  });
  method("setEndAfter", [set_point](NativeCall& call) {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node != nullptr && node->Parent() != nullptr) {
      set_point(call, false, node->Parent(), IndexIn(*node) + 1);
    }
    return Value::Undefined();
  });
  // `selectNode` puts the boundaries either side of the node; `selectNodeContents`
  // puts them inside it. One character of difference in the name and the whole
  // difference in what a selection covers.
  method("selectNode", [set_point](NativeCall& call) -> Value {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node == nullptr || node->Parent() == nullptr) {
      return call.Throw("Error", "InvalidNodeTypeError: the node has no parent");
    }
    const std::size_t index = IndexIn(*node);
    set_point(call, true, node->Parent(), index);
    set_point(call, false, node->Parent(), index + 1);
    return Value::Undefined();
  });
  method("selectNodeContents", [set_point](NativeCall& call) {
    dom::Node* node = NodeOf(Argument(call.arguments, 0));
    if (node != nullptr) {
      set_point(call, true, node, 0);
      set_point(call, false, node, LengthOf(*node));
    }
    return Value::Undefined();
  });
  // `collapse(true)` moves the end to the start; `collapse()` and
  // `collapse(false)` move the start to the end. The default is `false`, which
  // is the one people get wrong.
  method("collapse", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Undefined();
    }
    const bool to_start = js::ToBoolean(Argument(call.arguments, 0));
    const char* from_node = to_start ? kStartNodeSlot : kEndNodeSlot;
    const char* from_offset = to_start ? kStartOffsetSlot : kEndOffsetSlot;
    const Value* node = call.self.object->GetOwn(from_node);
    const Value* offset = call.self.object->GetOwn(from_offset);
    if (node == nullptr || offset == nullptr) {
      return Value::Undefined();
    }
    call.self.object->SetHidden(kStartNodeSlot, *node);
    call.self.object->SetHidden(kStartOffsetSlot, *offset);
    call.self.object->SetHidden(kEndNodeSlot, *node);
    call.self.object->SetHidden(kEndOffsetSlot, *offset);
    return Value::Undefined();
  });

  const auto boundary = [](const char* node_slot, const char* offset_slot, bool wants_node) {
    return [node_slot, offset_slot, wants_node](NativeCall& call) -> Value {
      DomBindings* owner = OwnerOf(call);
      if (owner == nullptr) {
        return Value::Null();
      }
      if (!wants_node) {
        return Value::Number(static_cast<double>(OffsetSlot(call.self, offset_slot)));
      }
      return owner->WrapperFor(NodeSlot(call.self, node_slot));
    };
  };
  accessor("startContainer", boundary(kStartNodeSlot, kStartOffsetSlot, true));
  accessor("startOffset", boundary(kStartNodeSlot, kStartOffsetSlot, false));
  accessor("endContainer", boundary(kEndNodeSlot, kEndOffsetSlot, true));
  accessor("endOffset", boundary(kEndNodeSlot, kEndOffsetSlot, false));

  accessor("collapsed", [](NativeCall& call) {
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    dom::Node* end = NodeSlot(call.self, kEndNodeSlot);
    if (start == nullptr || end == nullptr) {
      return Value::Bool(true);
    }
    return Value::Bool(ComparePoints(*start, OffsetSlot(call.self, kStartOffsetSlot), *end,
                                     OffsetSlot(call.self, kEndOffsetSlot)) == 0);
  });

  accessor("commonAncestorContainer", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    dom::Node* end = NodeSlot(call.self, kEndNodeSlot);
    if (owner == nullptr || start == nullptr || end == nullptr) {
      return Value::Null();
    }
    const std::vector<const dom::Node*> a = AncestorsOf(*start);
    const std::vector<const dom::Node*> b = AncestorsOf(*end);
    const dom::Node* common = nullptr;
    for (std::size_t i = 0; i < a.size() && i < b.size() && a[i] == b[i]; ++i) {
      common = a[i];
    }
    return owner->WrapperFor(const_cast<dom::Node*>(common));
  });

  // The reason this file exists, as far as the bundle that asked for it is
  // concerned: which of two boundary points comes first. `how` names which end
  // of each range to compare, and the four constants above are its values.
  method("compareBoundaryPoints", [](NativeCall& call) -> Value {
    const double how = js::ToNumber(Argument(call.arguments, 0));
    const Value other = Argument(call.arguments, 1);
    if (!call.self.IsObject() || !other.IsObject()) {
      return call.Throw("TypeError", "compareBoundaryPoints needs a Range");
    }
    const bool this_start = how == 0 || how == 1;
    const bool other_start = how == 0 || how == 3;
    dom::Node* a = NodeSlot(call.self, this_start ? kStartNodeSlot : kEndNodeSlot);
    dom::Node* b = NodeSlot(other, other_start ? kStartNodeSlot : kEndNodeSlot);
    if (a == nullptr || b == nullptr) {
      return call.Throw("TypeError", "compareBoundaryPoints needs two positioned Ranges");
    }
    const int order =
        ComparePoints(*a, OffsetSlot(call.self, this_start ? kStartOffsetSlot : kEndOffsetSlot),
                      *b, OffsetSlot(other, other_start ? kStartOffsetSlot : kEndOffsetSlot));
    if (order == 2) {
      return call.Throw("Error", "WrongDocumentError: the ranges are in different trees");
    }
    return Value::Number(order);
  });

  method("cloneRange", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr || !call.self.IsObject()) {
      return Value::Undefined();
    }
    const Value clone = call.interpreter.NewObjectValue();
    if (!clone.IsObject()) {
      return Value::Undefined();
    }
    clone.object->SetPrototype(call.self.object->Prototype());
    for (const char* slot : {kStartNodeSlot, kStartOffsetSlot, kEndNodeSlot, kEndOffsetSlot}) {
      if (const Value* found = call.self.object->GetOwn(slot)) {
        clone.object->SetHidden(slot, *found);
      }
    }
    return clone;
  });

  method("toString", [](NativeCall& call) {
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    dom::Node* end = NodeSlot(call.self, kEndNodeSlot);
    if (start == nullptr || end == nullptr) {
      return Value::String(std::string());
    }
    const std::vector<const dom::Node*> chain = AncestorsOf(*start);
    std::string out;
    if (!chain.empty()) {
      CollectText(*chain.front(), *start, OffsetSlot(call.self, kStartOffsetSlot), *end,
                  OffsetSlot(call.self, kEndOffsetSlot), out);
    }
    return Value::String(std::move(out));
  });

  // A no-op in the current specification, kept because old code calls it.
  method("detach", [](NativeCall&) { return Value::Undefined(); });

  // `document.createRange()`, which is how every one of these is made in
  // practice. `new Range()` works too and is what MakeInterface would
  // otherwise have made a TypeError -- so the constructor is replaced rather
  // than left as the illegal-constructor stub the other interfaces get.
  DomBindings* self = this;
  const Value constructor =
      interpreter_->NewNativeValue("Range", [self, range_interface](NativeCall& call) -> Value {
        const Value range = call.interpreter.NewObjectValue();
        if (!range.IsObject()) {
          return Value::Undefined();
        }
        range.object->SetPrototype(range_interface.object);
        // A fresh Range starts collapsed at the start of its document, which
        // is what the specification says and is why `createRange()` followed
        // by nothing is a valid, empty range rather than a broken one.
        dom::Node* document = self->document_;
        range.object->SetHidden(kStartNodeSlot, PointerValue(document));
        range.object->SetHidden(kStartOffsetSlot, Value::Number(0));
        range.object->SetHidden(kEndNodeSlot, PointerValue(document));
        range.object->SetHidden(kEndOffsetSlot, Value::Number(0));
        return range;
      });
  if (constructor.IsObject()) {
    constructor.object->Set("prototype", range_interface);
    for (const Constant& constant : kConstants) {
      constructor.object->Set(constant.name, Value::Number(constant.value));
    }
    range_interface.object->Set("constructor", constructor);
    interpreter_->Global()->Set("Range", constructor);
    interpreter_->GlobalScope()->Declare("Range", constructor, false);
  }

  const Value create = interpreter_->NewNativeValue("createRange", [](NativeCall& call) -> Value {
    DomBindings* owner = OwnerOf(call);
    if (owner == nullptr) {
      return Value::Undefined();
    }
    const Value* range_class = call.interpreter.Global()->GetOwn("Range");
    if (range_class == nullptr || !range_class->IsObject()) {
      return Value::Undefined();
    }
    const js::Result made = call.interpreter.ConstructValue(*range_class, {});
    if (made.completion == js::Completion::Throw) {
      return call.ThrowValue(made.value);
    }
    return made.value;
  });
  if (create.IsObject()) {
    create.object->Set(kOwnerSlot, PointerValue(this));
    const Value document_interface = DocumentInterface();
    if (document_interface.IsObject()) {
      document_interface.object->Set("createRange", create);
    }
  }
}

}  // namespace microbrowser::bindings
