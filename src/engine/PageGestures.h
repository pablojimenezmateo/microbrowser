#pragma once

#include <optional>
#include <string>

#include "gfx/Geometry.h"

namespace microbrowser::dom {
class Element;
}

namespace microbrowser::layout {
class Box;
}

namespace microbrowser::engine {

// What a gesture at a point resolved to, before anything acts on it.
//
// Split out of Page.h, which reached the module's line cap -- and the cap was pointing at
// something real. None of this is *about* a page: it is the vocabulary between "the user pressed
// here" and "so do that", and the two halves are deliberately separate types. A hit test answers
// where; a dispatch answers what the page's own handlers did with it; an activation answers what
// the user agent still owes. Fusing them is how a default action ends up running on a page that
// called `preventDefault`.

// What dispatching an event did, which is two separate facts.
//
// A handler that changed the document needs a relayout whether or not it
// prevented anything, and a handler that prevented the default may have
// changed nothing at all. Reporting one bit conflated the two, and the visible
// symptom was a page whose handler ran and whose screen did not change.
struct FormSubmission {
  std::string url;
  std::string method = "GET";
  std::string body;
  std::string content_type;
};

struct DispatchOutcome {
  bool ran = false;
  bool prevented = false;
  // Element the `click` event targeted (UI Events common ancestor of press and
  // release). Default actions must walk *this* rather than re-hit-testing the
  // point: a dialog that removes itself on mousedown would otherwise leave the
  // release on whatever was underneath (youtube Accept → search result).
  dom::Element* click_target = nullptr;
};

// What a completed primary click's default action should do, resolved from the
// click target rather than from a fresh hit-test at the pointer.
struct ClickActivation {
  std::optional<FormSubmission> form;
  std::optional<std::string> href;
  bool reset_form = false;
  bool toggled_checkable = false;
  bool toggled_media = false;
};

// Form-control hit test shared by Page's click default actions. Lives in
// PageHitTest.cpp with LinkAt / ElementAt so visibility:hidden is one walk.
dom::Element* HitTestFormControlAt(const layout::Box& root, gfx::FloatPoint document_point,
                                   bool (*predicate)(const dom::Element&),
                                   float document_scroll_y = 0.0f);

}  // namespace microbrowser::engine
