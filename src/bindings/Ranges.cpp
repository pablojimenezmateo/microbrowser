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
// The tree surgery -- `extractContents`, `deleteContents`, `cloneContents`,
// `insertNode` and `surroundContents` -- is next door in RangeContents.cpp.
// It was absent until 2026-08-11 and ADR 0012 was right about why: the
// partial-containment case is the algorithm, not a corner of it.
//
// **A Range here is live.** Its boundary points move when the tree moves --
// inserting before the start bumps the offset, removing the node a boundary is
// inside collapses that boundary onto the gap. The registry and the four fixup
// rules are in BindingSupport.h, next to the tree-order function they are
// written against, because the mutation primitives that must call them are in
// three other translation units.
//
// The **refusals** here are load-bearing and were the single biggest thing
// missing: `setStart` and friends used to drop a bad argument on the floor, so
// a page that set a boundary out of range got a range still pointing wherever
// it pointed before and read a plausible wrong answer off it. Every method that
// takes a boundary point now throws IndexSizeError or InvalidNodeTypeError, in
// that order, through the one `BoundaryPointError` below.

#include <cstdint>
#include <string>
#include <vector>

#include "bindings/BindingSupport.h"
#include "bindings/DomBindings.h"
#include "bindings/LiveRanges.h"
#include "bindings/WebIdl.h"

namespace microbrowser::bindings {

using js::NativeCall;
using js::Value;

namespace {

// **The one argument check every boundary-taking method owes.**
//
// `setStart`, `setEnd`, `comparePoint`, `isPointInRange` and
// `selectNodeContents` all begin with the same two refusals, in the same
// order: a doctype is not a valid container, and an offset past the node's
// length is out of range. Writing it once is not tidiness -- the order is
// observable, because a doctype has length zero and would otherwise report
// IndexSizeError for a fault the specification calls InvalidNodeTypeError.
//
// Null when the point is usable, otherwise the DOMException name to throw.
const char* BoundaryPointError(const dom::Node& node, std::size_t offset) {
  if (node.GetKind() == dom::Node::Kind::DocumentType) {
    return "InvalidNodeTypeError";
  }
  if (offset > NodeLength(node)) {
    return "IndexSizeError";
  }
  return nullptr;
}

// The (node, offset) pair a method was handed, converted and validated.
//
// `offset` is a WebIDL `unsigned long`, so it *wraps* rather than clamping --
// `setStart(node, -1)` is offset 4294967295 and therefore an IndexSizeError,
// which is exactly what the tests assert and is not what a `size_t` cast of a
// double would have produced.
bool ToBoundaryPoint(NativeCall& call, const char* operation, dom::Node*& node_out,
                     std::uint32_t& offset_out) {
  if (!RequireArguments(call, "Range", operation, 2)) {
    return false;
  }
  node_out = NodeOf(call.arguments[0]);
  if (node_out == nullptr) {
    call.Throw("TypeError", std::string(operation) + " needs a Node");
    return false;
  }
  return ToUnsignedLong(call, call.arguments[1], IntegerRange::Modulo, offset_out);
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
      native.object->Set(kOwnerSlot, OwnerValue(this));
      range_interface.object->Set(name, native);
    }
  };
  const auto accessor = [this, &range_interface](const char* name, js::NativeFunction get) {
    const Value native = interpreter_->NewNativeValue(name, std::move(get));
    if (native.IsObject()) {
      native.object->Set(kOwnerSlot, OwnerValue(this));
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

  // `setStart` and `setEnd` **refuse** rather than silently doing nothing.
  // Until 2026-08-11 both dropped a bad argument on the floor, which is the
  // worst of the three options: a page that set a boundary out of range got a
  // range still pointing at wherever it pointed before, and read a plausible
  // wrong answer off it instead of catching an exception.
  const auto set_validated = [set_point](NativeCall& call, bool start,
                                         const char* operation) -> Value {
    dom::Node* node = nullptr;
    std::uint32_t offset = 0;
    if (!ToBoundaryPoint(call, operation, node, offset)) {
      return call.ThrownValue();
    }
    if (const char* error = BoundaryPointError(*node, offset); error != nullptr) {
      return ThrowDom(call, error, std::string(operation) + " was given a boundary point that " +
                                       "is not in range for that node");
    }
    set_point(call, start, node, offset);
    return Value::Undefined();
  };
  method("setStart", [set_validated](NativeCall& call) {
    return set_validated(call, true, "setStart");
  });
  method("setEnd", [set_validated](NativeCall& call) {
    return set_validated(call, false, "setEnd");
  });

  // The four `set*Before` / `set*After`. A node with no parent has no position
  // among siblings to name, so these are InvalidNodeTypeError rather than
  // no-ops -- the same refusal `selectNode` owes for the same reason.
  const auto set_beside = [set_point](NativeCall& call, bool start, bool after,
                                      const char* operation) -> Value {
    if (!RequireArguments(call, "Range", operation, 1)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[0]);
    if (node == nullptr) {
      return call.Throw("TypeError", std::string(operation) + " needs a Node");
    }
    dom::Node* parent = node->Parent();
    if (parent == nullptr) {
      return ThrowDom(call, "InvalidNodeTypeError", "the node has no parent");
    }
    set_point(call, start, parent, IndexIn(*node) + (after ? 1 : 0));
    return Value::Undefined();
  };
  method("setStartBefore", [set_beside](NativeCall& call) {
    return set_beside(call, true, false, "setStartBefore");
  });
  method("setStartAfter", [set_beside](NativeCall& call) {
    return set_beside(call, true, true, "setStartAfter");
  });
  method("setEndBefore", [set_beside](NativeCall& call) {
    return set_beside(call, false, false, "setEndBefore");
  });
  method("setEndAfter", [set_beside](NativeCall& call) {
    return set_beside(call, false, true, "setEndAfter");
  });
  // `selectNode` puts the boundaries either side of the node; `selectNodeContents`
  // puts them inside it. One character of difference in the name and the whole
  // difference in what a selection covers.
  method("selectNode", [set_point](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Range", "selectNode", 1)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[0]);
    if (node == nullptr) {
      return call.Throw("TypeError", "selectNode needs a Node");
    }
    if (node->Parent() == nullptr) {
      return ThrowDom(call, "InvalidNodeTypeError", "the node has no parent");
    }
    const std::size_t index = IndexIn(*node);
    set_point(call, true, node->Parent(), index);
    set_point(call, false, node->Parent(), index + 1);
    return Value::Undefined();
  });
  method("selectNodeContents", [set_point](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Range", "selectNodeContents", 1)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[0]);
    if (node == nullptr) {
      return call.Throw("TypeError", "selectNodeContents needs a Node");
    }
    // A doctype has no contents to select. Its *length* is zero, so without
    // this the call would quietly make an empty range instead of throwing.
    if (node->GetKind() == dom::Node::Kind::DocumentType) {
      return ThrowDom(call, "InvalidNodeTypeError", "a doctype has no contents");
    }
    set_point(call, true, node, 0);
    set_point(call, false, node, NodeLength(*node));
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
      return ThrowDom(call, "WrongDocumentError", "the ranges are in different trees");
    }
    return Value::Number(order);
  });

  // **Where is this point, relative to the range?** Three methods, one
  // algorithm, and between them 12,050 of `dom/ranges/`'s failing subtests --
  // more than every content-mutation method combined. They were absent
  // together and they land together, because `comparePoint` *is*
  // `isPointInRange` with the answer spelled differently, and answering the two
  // from separate code is how they end up disagreeing about a boundary.
  //
  // The refusals differ, and that difference is the whole of what the tests
  // check: a point in another tree is a WrongDocumentError to `comparePoint`
  // and a plain `false` to `isPointInRange`, because asking "where" about a
  // point that has no position is an error while asking "is it inside" is not.
  const auto point_against_range = [](NativeCall& call, const char* operation,
                                      bool throws_on_other_root, int& order_out,
                                      bool& other_root_out) -> bool {
    dom::Node* node = nullptr;
    std::uint32_t offset = 0;
    if (!ToBoundaryPoint(call, operation, node, offset)) {
      return false;
    }
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    dom::Node* end = NodeSlot(call.self, kEndNodeSlot);
    if (start == nullptr || end == nullptr) {
      call.Throw("TypeError", std::string(operation) + " needs a positioned Range");
      return false;
    }
    // **Root first, and it terminates the algorithm.** The order is the
    // specification's and it is observable both ways: `comparePoint` on a
    // point in another tree is a WrongDocumentError even when the offset is
    // also out of range, and `isPointInRange` answers a plain `false` there
    // *without* going on to check the offset -- so `isPointInRange(nodeInOtherTree, -1)`
    // is false rather than an IndexSizeError. Checking the offset first cost
    // 871 subtests.
    other_root_out = RootOf(*node) != RootOf(*start);
    if (other_root_out) {
      if (throws_on_other_root) {
        ThrowDom(call, "WrongDocumentError", "the point is in a different tree from the range");
        return false;
      }
      order_out = 0;
      return true;
    }
    if (const char* error = BoundaryPointError(*node, offset); error != nullptr) {
      ThrowDom(call, error, std::string(operation) + " was given a point that is not valid");
      return false;
    }
    // Before the start is -1, after the end is 1, anywhere between is 0.
    order_out = ComparePoints(*node, offset, *start, OffsetSlot(call.self, kStartOffsetSlot)) < 0
                    ? -1
                    : (ComparePoints(*node, offset, *end, OffsetSlot(call.self, kEndOffsetSlot)) > 0
                           ? 1
                           : 0);
    return true;
  };

  method("comparePoint", [point_against_range](NativeCall& call) -> Value {
    int order = 0;
    bool other_root = false;
    if (!point_against_range(call, "comparePoint", true, order, other_root)) {
      return call.ThrownValue();
    }
    return Value::Number(order);
  });

  method("isPointInRange", [point_against_range](NativeCall& call) -> Value {
    int order = 0;
    bool other_root = false;
    if (!point_against_range(call, "isPointInRange", false, order, other_root)) {
      return call.ThrownValue();
    }
    // A point in another tree is simply not in this range -- and note it is
    // `false` *after* the node-type and offset checks have had their say, so
    // `isPointInRange(doctype, 0)` still throws.
    return Value::Bool(!other_root && order == 0);
  });

  // `intersectsNode` asks about a whole node rather than a point, and is not
  // expressible as two `comparePoint`s: a node that merely *contains* the range
  // intersects it, which is why the test is against the node's own two sides
  // rather than against its interior.
  method("intersectsNode", [](NativeCall& call) -> Value {
    if (!RequireArguments(call, "Range", "intersectsNode", 1)) {
      return call.ThrownValue();
    }
    dom::Node* node = NodeOf(call.arguments[0]);
    if (node == nullptr) {
      return call.Throw("TypeError", "intersectsNode needs a Node");
    }
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    dom::Node* end = NodeSlot(call.self, kEndNodeSlot);
    if (start == nullptr || end == nullptr) {
      return call.Throw("TypeError", "intersectsNode needs a positioned Range");
    }
    if (RootOf(*node) != RootOf(*start)) {
      return Value::Bool(false);
    }
    dom::Node* parent = node->Parent();
    if (parent == nullptr) {
      // A root has no position among siblings, and the range is inside it by
      // construction -- they share a root and this node *is* it.
      return Value::Bool(true);
    }
    const std::size_t index = IndexIn(*node);
    const bool before_end =
        ComparePoints(*parent, index, *end, OffsetSlot(call.self, kEndOffsetSlot)) < 0;
    const bool after_start =
        ComparePoints(*parent, index + 1, *start, OffsetSlot(call.self, kStartOffsetSlot)) > 0;
    return Value::Bool(before_end && after_start);
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
    RegisterLiveRange(call.interpreter, clone);
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

  // CSSOM View: a Range's boxes are the boxes of the nodes it covers. One
  // fragment today, from the start container's element — enough for
  // `assert_class_string` and for a collapsed range on an element to answer.
  method("getClientRects", [](NativeCall& call) -> Value {
    std::vector<Value> rects;
    DomBindings* owner = OwnerOf(call);
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    while (start != nullptr && !start->IsElement()) {
      start = start->Parent();
    }
    if (owner != nullptr && owner->geometry_ != nullptr && start != nullptr) {
      if (const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*start)) {
        rects.push_back(MakeDomRect(call.interpreter, found->border_box));
      }
    }
    return MakeDomRectList(call.interpreter, std::move(rects));
  });
  method("getBoundingClientRect", [](NativeCall& call) -> Value {
    GeometryRect box;
    DomBindings* owner = OwnerOf(call);
    dom::Node* start = NodeSlot(call.self, kStartNodeSlot);
    while (start != nullptr && !start->IsElement()) {
      start = start->Parent();
    }
    if (owner != nullptr && owner->geometry_ != nullptr && start != nullptr) {
      if (const std::optional<BoxGeometry> found = owner->geometry_->QueryBox(*start)) {
        box = found->border_box;
      }
    }
    return MakeDomRect(call.interpreter, box);
  });

  // The five that change the tree, in RangeContents.cpp.
  InstallRangeContents(range_interface);

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
        // From here on the tree moves it. A Range that is not on this list is
        // a snapshot, which is what every Range in this browser used to be.
        RegisterLiveRange(call.interpreter, range);
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
    // Collapsed at the start of **the document that was asked**, not of the
    // page's own. `new Range()` uses the current global's document, which is
    // what the constructor above does; `doc.createRange()` has to answer a
    // range whose `startContainer` is `doc`, and a second document -- one from
    // `createHTMLDocument` or `new Document()` -- is exactly where the two
    // differ.
    if (made.value.IsObject()) {
      const Value document = PointerValue(owner->DocumentOf(call.self));
      made.value.object->SetHidden(kStartNodeSlot, document);
      made.value.object->SetHidden(kEndNodeSlot, document);
    }
    return made.value;
  });
  if (create.IsObject()) {
    create.object->Set(kOwnerSlot, OwnerValue(this));
    const Value document_interface = DocumentInterface();
    if (document_interface.IsObject()) {
      document_interface.object->Set("createRange", create);
    }
  }

  InstallStaticRange();
}

// `StaticRange`: the same two boundary points, and deliberately none of the
// behaviour.
//
// It is not a cut-down Range and it is not an optimisation. A Range is *live* --
// the tree moving moves it, which costs every mutation a walk of the registry --
// and a StaticRange is a **record of where two points were**, which is what an
// `input` event's `getTargetRanges()` hands a page and what a page keeps when it
// wants to remember a selection without pinning the tree. So the two share the
// slot names and nothing else: no live-range registration, no revalidation, and
// no refusal for an offset past the node's length, because a StaticRange whose
// endpoints have since moved is a valid StaticRange.
//
// The one refusal it does have is on the *kind* of node: a DocumentType or an
// Attr cannot be a container, which is the check `Range` makes too and for the
// same reason -- neither is a node a boundary point can be inside.
void DomBindings::InstallStaticRange() {
  const Value prototype = MakeInterface("StaticRange", Value::Undefined());
  if (!prototype.IsObject()) {
    return;
  }
  struct Endpoint {
    const char* property;
    const char* member;
    const char* slot;
    bool node;
  };
  static constexpr Endpoint kEndpoints[] = {
      {"startContainer", "startContainer", kStartNodeSlot, true},
      {"startOffset", "startOffset", kStartOffsetSlot, false},
      {"endContainer", "endContainer", kEndNodeSlot, true},
      {"endOffset", "endOffset", kEndOffsetSlot, false},
  };
  for (const Endpoint& endpoint : kEndpoints) {
    const char* slot = endpoint.slot;
    const bool is_node = endpoint.node;
    const Value getter =
        interpreter_->NewNativeValue(endpoint.property, [slot, is_node](NativeCall& call) -> Value {
          DomBindings* owner = OwnerOf(call);
          const Value* stored = call.self.IsObject() ? call.self.object->GetOwn(slot) : nullptr;
          if (stored == nullptr) {
            return Value::Undefined();
          }
          if (!is_node) {
            return *stored;
          }
          auto* node = reinterpret_cast<dom::Node*>(static_cast<std::uintptr_t>(stored->number));
          return owner == nullptr || node == nullptr ? Value::Null() : owner->WrapperFor(node);
        });
    if (getter.IsObject()) {
      getter.object->Set(kOwnerSlot, OwnerValue(this));
      prototype.object->DefineAccessor(endpoint.property, getter.object, nullptr);
    }
  }
  const Value collapsed = interpreter_->NewNativeValue("collapsed", [](NativeCall& call) {
    if (!call.self.IsObject()) {
      return Value::Bool(false);
    }
    const Value* start_node = call.self.object->GetOwn(kStartNodeSlot);
    const Value* end_node = call.self.object->GetOwn(kEndNodeSlot);
    const Value* start_offset = call.self.object->GetOwn(kStartOffsetSlot);
    const Value* end_offset = call.self.object->GetOwn(kEndOffsetSlot);
    return Value::Bool(start_node != nullptr && end_node != nullptr &&
                       start_node->number == end_node->number && start_offset != nullptr &&
                       end_offset != nullptr && start_offset->number == end_offset->number);
  });
  if (collapsed.IsObject()) {
    prototype.object->DefineAccessor("collapsed", collapsed.object, nullptr);
  }

  const Value constructor =
      interpreter_->NewNativeValue("StaticRange", [prototype](NativeCall& call) -> Value {
        if (!RequireArguments(call, "StaticRange", "constructor", 1) ||
            !call.arguments[0].IsObject()) {
          return call.HasThrown() ? call.ThrownValue()
                                  : call.Throw("TypeError",
                                               "StaticRange requires a StaticRangeInit");
        }
        js::Object* init = call.arguments[0].object;
        struct Member {
          const char* name;
          const char* slot;
          bool node;
        };
        static constexpr Member kMembers[] = {
            {"startContainer", kStartNodeSlot, true},
            {"startOffset", kStartOffsetSlot, false},
            {"endContainer", kEndNodeSlot, true},
            {"endOffset", kEndOffsetSlot, false},
        };
        const Value range = call.interpreter.NewObjectValue();
        if (!range.IsObject()) {
          return Value::Undefined();
        }
        range.object->SetPrototype(prototype.object);
        for (const Member& member : kMembers) {
          const Value* given = init->Get(member.name);
          // **Every member of StaticRangeInit is `required`**, so an absent one
          // is a TypeError rather than a zero. WebIDL treats `undefined` as
          // absent, which is why the test is on the value and not only on the
          // key -- `{startOffset: undefined}` is a dictionary missing its
          // `startOffset`, and a range silently starting at 0 instead is a
          // wrong answer where the page asked a wrong question.
          if (given == nullptr || given->IsUndefined()) {
            return call.Throw("TypeError", std::string("StaticRangeInit is missing ") +
                                               member.name);
          }
          if (!member.node) {
            std::uint32_t offset = 0;
            if (!ToUnsignedLong(call, *given, IntegerRange::Modulo, offset)) {
              return call.ThrownValue();
            }
            range.object->SetHidden(member.slot, Value::Number(static_cast<double>(offset)));
            continue;
          }
          dom::Node* node = NodeOf(*given);
          if (node == nullptr) {
            return call.Throw("TypeError",
                              std::string(member.name) + " is not a Node");
          }
          if (node->GetKind() == dom::Node::Kind::DocumentType) {
            return ThrowDom(call, "InvalidNodeTypeError",
                            "a DocumentType cannot be a boundary point's container");
          }
          range.object->SetHidden(member.slot, PointerValue(node));
        }
        return range;
      });
  if (constructor.IsObject()) {
    constructor.object->Set("prototype", prototype);
    prototype.object->Set("constructor", constructor);
    interpreter_->GlobalScope()->Declare("StaticRange", constructor, false);
  }
}

}  // namespace microbrowser::bindings
