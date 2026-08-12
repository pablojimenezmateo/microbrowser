// The child browsing contexts of one document. ADR 0027 §1.
//
// Lifted out of PageResources.cpp, which is about *what else does this document need fetching*.
// A frame is that too, but it is also a second document with a lifetime of its own, and the two
// halves were being read as one.

#include "engine/Frames.h"

#include <algorithm>
#include <utility>

#include "engine/Page.h"
#include "util/PerformanceCounters.h"

namespace microbrowser::engine {

namespace {

using util::AddPerformanceCounter;
using util::PerfCounterId;

// What this element is asking to load, as written. `srcdoc` wins over `src`, which is what HTML
// says and is the only reason to read both here rather than at the loader: the loader sees a URL
// and `srcdoc` is not one.
std::string RequestedSourceOf(const dom::Element& element) {
  if (const std::string* srcdoc = element.GetAttribute("srcdoc"); srcdoc != nullptr) {
    return "srcdoc:" + *srcdoc;
  }
  if (const std::string* src = element.GetAttribute("src"); src != nullptr) {
    return "src:" + *src;
  }
  return "src:";
}

}  // namespace

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

FrameTree::FrameTree() = default;

// Out of line, and it has to be: `Frame` holds a `unique_ptr<Page>` and `Page` is incomplete in
// the header. This is also where a whole subtree of contexts is destroyed, children first.
FrameTree::~FrameTree() { Clear(); }

bool FrameTree::NeedsCollect(const dom::Document* document) const {
  if (document == nullptr) {
    return !frames_.empty();
  }
  if (!ever_collected_ || document->StructureVersion() != collected_structure_version_) {
    return true;
  }
  // **The structure version cannot see `iframe.src = other`.** That is an attribute write: it
  // moves the mutation version and leaves the structure alone, which is exactly the distinction
  // TD-0021 drew and exactly the wrong side of it here -- assigning `src` is a *navigation*, and
  // gating the whole pass on structure alone made it silently do nothing.
  //
  // So the gate is two questions rather than one, and the second is answered without walking the
  // document: this is a loop over the frames that already exist, which is the count of `<iframe>`
  // elements on the page rather than the count of nodes. A page with no frames pays the integer
  // comparison above and nothing else.
  for (const Frame& frame : frames_) {
    if (frame.element != nullptr && frame.requested &&
        RequestedSourceOf(*frame.element) != frame.requested_source) {
      return true;
    }
  }
  return false;
}

std::vector<std::size_t> FrameTree::Collect(
    dom::Document* document, const std::function<std::unique_ptr<Page>()>& make_page) {
  std::vector<std::size_t> wanted;
  if (document == nullptr) {
    Clear();
    ever_collected_ = false;
    return wanted;
  }
  ever_collected_ = true;
  collected_structure_version_ = document->StructureVersion();
  AddPerformanceCounter(PerfCounterId::EngineFramesCollected);
  const std::vector<dom::Element*> elements = CollectFrameElements(*document);

  // **A surviving element keeps its context.** Rebuilding the list from scratch on every collection
  // would reload every frame on the page each time a script appended a `<div>` -- and a frame that
  // reloads on an unrelated mutation is a page that never settles, plus a request per mutation
  // going out to whoever the frame points at.
  std::vector<Frame> kept;
  kept.reserve(elements.size());
  for (dom::Element* element : elements) {
    auto existing = std::find_if(frames_.begin(), frames_.end(),
                                 [&](const Frame& frame) { return frame.element == element; });
    if (existing != frames_.end()) {
      // **A frame whose element now names something else is a navigation.** `iframe.src = other`
      // has to reload; anything that did not touch `src` or `srcdoc` must not. Comparing against
      // what was last *requested* is what tells those apart -- and the request is dropped rather
      // than cancelled, because the in-flight one is keyed by index and the index survives.
      if (existing->requested && RequestedSourceOf(*element) != existing->requested_source) {
        existing->requested = false;
        existing->loaded = false;
        existing->page = make_page();
        element->SetNestedDocument(nullptr);
        AddPerformanceCounter(PerfCounterId::EngineFramesRenavigated);
      }
      kept.push_back(std::move(*existing));
      // Moved out, so the loop below does not clear the element's pointer for a frame that is
      // still alive. `element` is nulled rather than the whole entry erased because erasing from
      // the middle of a vector being iterated is the other way to get this wrong.
      existing->element = nullptr;
      existing->page.reset();
      continue;
    }
    Frame frame;
    frame.element = element;
    frame.page = make_page();
    kept.push_back(std::move(frame));
    AddPerformanceCounter(PerfCounterId::EngineFramesCreated);
  }
  // Whatever is left in `frames_` is a frame whose element is gone from the document. It ends here,
  // and it ends through the same clear-then-drop the destructor path uses.
  //
  // **`DropFrames` rather than `Clear`**, and the difference is a bug that was already written
  // once: `Clear` also empties the owed-`load` queue, and this pass runs between a frame being
  // handed its document and the event being dispatched. Every `<iframe srcdoc>` in the suite goes
  // through exactly that window -- the document is set during the subresource pass, the page's
  // script runs and moves the structure version, and the recollection that follows swallowed the
  // event. What has to go instead is only the entries naming elements that just died.
  DropFrames();
  frames_ = std::move(kept);
  ForgetLoadEventsNotIn(frames_);

  for (std::size_t index = 0; index < frames_.size(); ++index) {
    if (!frames_[index].loaded) {
      wanted.push_back(index);
    }
  }
  return wanted;
}

void FrameTree::MarkRequested(std::size_t index) {
  if (index >= frames_.size()) {
    return;
  }
  Frame& frame = frames_[index];
  frame.requested = true;
  // **Recorded when the request goes out, not when the answer arrives**, and that ordering is the
  // whole correctness of `NeedsCollect`'s second question: a frame in flight has no document yet,
  // so a key recorded only at delivery would differ from the element's for the entire round trip
  // -- and every collection pass in that window would read it as a fresh navigation and start the
  // request again. Which is a request per turn, to whoever the frame points at.
  if (frame.element != nullptr) {
    frame.requested_source = RequestedSourceOf(*frame.element);
  }
}

void FrameTree::SetDocument(std::size_t index, std::string_view html, std::string url,
                            csp::PolicyList header_policy, std::string_view content_type,
                            bool same_origin) {
  if (index >= frames_.size() || frames_[index].page == nullptr) {
    return;
  }
  Frame& frame = frames_[index];
  frame.url = url;
  frame.same_origin = same_origin;
  if (frame.element != nullptr) {
    frame.requested_source = RequestedSourceOf(*frame.element);
  }
  frame.page->Load(html, std::move(url), std::move(header_policy), content_type);
  frame.loaded = true;
  // The origin check, and it is the whole of it: a cross-origin child has no document on its
  // element, so `iframe.contentDocument` has nothing to return. Nothing above this module has to
  // remember to ask. ADR 0027 §2.
  if (frame.element != nullptr) {
    frame.element->SetNestedDocument(same_origin ? frame.page->MutableDocument() : nullptr);
    // **`load` fires even for a cross-origin child**, and that is not an oversight: HTML fires it
    // on the *element*, in the embedder, and a page that could tell a cross-origin load from a
    // failure by whether the event arrived would have a cross-origin oracle. The event carries no
    // information about what loaded.
    pending_load_events_.push_back(frame.element);
  }
  AddPerformanceCounter(PerfCounterId::EngineFramesLoaded);
}

bool FrameTree::AllLoaded() const {
  for (const Frame& frame : frames_) {
    if (!frame.loaded) {
      return false;
    }
    if (frame.page != nullptr && !frame.page->FramesLoaded()) {
      return false;  // a frame inside a frame; the load event waits for the whole subtree
    }
  }
  return true;
}

std::vector<dom::Element*> FrameTree::TakePendingLoadEvents() {
  std::vector<dom::Element*> taken;
  taken.swap(pending_load_events_);
  return taken;
}

void FrameTree::DropFrames() {
  // The borrowed pointer goes *before* the page that owns the document it points at. Both halves
  // are here rather than at any caller, because a second place that dropped a frame would be a
  // use-after-free a page could drive with `iframe.remove()`. See the header.
  for (Frame& frame : frames_) {
    if (frame.element != nullptr) {
      frame.element->SetNestedDocument(nullptr);
    }
  }
  frames_.clear();
}

void FrameTree::ForgetLoadEventsNotIn(const std::vector<Frame>& live) {
  const auto still_live = [&live](const dom::Element* element) {
    return std::any_of(live.begin(), live.end(),
                       [element](const Frame& frame) { return frame.element == element; });
  };
  pending_load_events_.erase(
      std::remove_if(pending_load_events_.begin(), pending_load_events_.end(),
                     [&](dom::Element* element) { return !still_live(element); }),
      pending_load_events_.end());
}

void FrameTree::Clear() {
  DropFrames();
  // The elements queued here live in the document whose frames these were. Dropping the queue with
  // the frames is what stops a `load` event being dispatched at an element a navigation freed.
  pending_load_events_.clear();
}

}  // namespace microbrowser::engine
