#include "engine/Frames.h"

namespace microbrowser::engine {

std::vector<dom::Element*> CollectFrameElements(dom::Document& document) {
  std::vector<dom::Element*> found;
  document.ForEachDescendant([&](const dom::Node& node) {
    if (!node.IsElement()) {
      return;
    }
    // `<frame>` and `<frameset>` are deliberately absent: they are removed from the specification,
    // and the pages that use them were already broken before this browser existed. ADR 0027 §6.
    const auto& element = static_cast<const dom::Element&>(node);
    if (element.TagName() == "iframe") {
      found.push_back(const_cast<dom::Element*>(&element));
    }
  });
  return found;
}

}  // namespace microbrowser::engine
