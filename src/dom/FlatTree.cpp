#include "dom/FlatTree.h"

namespace microbrowser::dom {

namespace {

bool IsSlot(const Node& node) {
  return node.IsElement() && static_cast<const Element&>(node).TagName() == "slot";
}

// The `slot=` a light-DOM child asks for, or empty for the unnamed slot.
std::string_view SlotNameOf(const Node& node) {
  if (!node.IsElement()) {
    // A text node has no attributes, so it always goes to the unnamed slot --
    // which is why `<my-el>text</my-el>` fills a default `<slot>` and nothing
    // else.
    return {};
  }
  const std::string* name = static_cast<const Element&>(node).GetAttribute("slot");
  return name == nullptr ? std::string_view{} : std::string_view(*name);
}

std::string_view NameOf(const Element& slot) {
  const std::string* name = slot.GetAttribute("name");
  return name == nullptr ? std::string_view{} : std::string_view(*name);
}

}  // namespace

const Element* ShadowHostOf(const Node& node) {
  const Node* root = &node;
  while (root->Parent() != nullptr) {
    root = root->Parent();
  }
  if (root->GetKind() != Node::Kind::DocumentFragment) {
    return nullptr;
  }
  return static_cast<const DocumentFragment*>(root)->Host();
}

std::vector<Node*> AssignedNodes(const Element& slot) {
  std::vector<Node*> assigned;
  const Element* host = ShadowHostOf(slot);
  if (host == nullptr) {
    return assigned;
  }
  const std::string_view wanted = NameOf(slot);
  for (const std::unique_ptr<Node>& child : host->Children()) {
    if (child == nullptr) {
      continue;
    }
    // A slot is filled from the *host's* children and from nothing else -- not
    // from its descendants, which is the rule that keeps assignment a single
    // level and makes it computable without a walk.
    if (SlotNameOf(*child) == wanted) {
      assigned.push_back(child.get());
    }
  }
  return assigned;
}

std::vector<Node*> FlatChildren(const Node& node) {
  std::vector<Node*> children;
  if (node.IsElement()) {
    const auto& element = static_cast<const Element&>(node);
    if (DocumentFragment* shadow = element.ShadowRoot()) {
      // A host is replaced by its shadow root's children. Its own children -- the
      // light DOM -- are reached only through the slots inside that root, which
      // is why a host with a shadow tree and no `<slot>` renders nothing of what
      // is written inside it. Every framework depends on exactly that.
      for (const std::unique_ptr<Node>& child : shadow->Children()) {
        if (child != nullptr) {
          children.push_back(child.get());
        }
      }
      return children;
    }
    if (IsSlot(element)) {
      std::vector<Node*> assigned = AssignedNodes(element);
      if (!assigned.empty()) {
        return assigned;
      }
      // Nothing was assigned, so the slot's own children are its fallback
      // content. This is the case a materialised flat tree gets wrong most often:
      // the fallback is *conditional*, so a tree built once is stale the moment a
      // matching child is added.
    }
  }
  // No skipping is needed for a slotted child, and that is worth stating because
  // it looks like it should be: a host's light children are never *reached* as
  // flat children, since the host answered with its shadow root's children above.
  // They appear exactly once, at the slot that claimed them.
  for (const std::unique_ptr<Node>& child : node.Children()) {
    if (child != nullptr) {
      children.push_back(child.get());
    }
  }
  return children;
}

bool HasShadowTrees(const Node& root) {
  if (root.IsElement() && static_cast<const Element&>(root).ShadowRoot() != nullptr) {
    return true;
  }
  for (const std::unique_ptr<Node>& child : root.Children()) {
    if (child != nullptr && HasShadowTrees(*child)) {
      return true;
    }
  }
  return false;
}

}  // namespace microbrowser::dom
