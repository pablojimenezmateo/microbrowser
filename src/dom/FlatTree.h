#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dom/Node.h"

namespace microbrowser::dom {

// The flattened tree, as a *traversal*.
//
// ADR 0019 §2. Two trees exist and they are not the same:
//
//   * the **node tree**, which is what the DOM answers about -- `parentNode`,
//     `childNodes`, `querySelector` within a root;
//   * the **flattened tree**, which is what layout and the cascade walk -- a
//     shadow host is replaced by its shadow root's children, and a `<slot>` is
//     replaced by the nodes assigned to it.
//
// It is deliberately **not materialised**. A parallel tree of real nodes would
// double the memory per element and create two things that can disagree, and a
// disagreement between the tree the DOM answers about and the tree that renders
// is the bug class that eats weeks. So this is one function -- *what are this
// node's children, for rendering* -- and everything that walks the document for
// layout or for style calls it instead of `Children()`.
//
// Slot assignment is computed here rather than stored, with one exception the ADR
// names: `slotchange` needs to know when an assignment *changed*, which is a
// comparison against what it was, so the binding layer keeps the previous
// answer. The traversal itself asks fresh every time, which is what keeps the two
// trees from being able to disagree.

// This node's children in the flattened tree.
//
// Not a reference: a host's flat children are its shadow root's, and a slot's are
// its assigned nodes, so for two of the three cases there is no existing vector
// to point at. The common case -- an element with no shadow root and no slots
// under one -- still copies a vector of pointers, which is the price of the two
// trees never disagreeing.
std::vector<Node*> FlatChildren(const Node& node);

// Whether anything in this document uses a shadow tree at all.
//
// Asked before the flat traversal is used, so a document with no shadow root
// pays one bool rather than a copied vector per element. The same shape ADR
// 0016's `StyleDependsOn` uses, and for the same reason.
bool HasShadowTrees(const Node& root);

// The nodes assigned to `slot`, in tree order.
//
// A slot is filled by the *host's* children -- the light DOM -- matched by name:
// a child with `slot="x"` goes to the slot named `x`, and a child with none goes
// to the unnamed slot. Nothing else is assigned, which is why a slot with no
// match falls back to its own children.
std::vector<Node*> AssignedNodes(const Element& slot);

// The host whose shadow tree contains `node`, or null. What event retargeting
// walks and what tells a slot which children it can be filled from.
const Element* ShadowHostOf(const Node& node);

}  // namespace microbrowser::dom
