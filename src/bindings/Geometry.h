#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace microbrowser::dom {
class Element;
class Node;
}  // namespace microbrowser::dom

namespace microbrowser::bindings {

// A rectangle in CSS pixels, relative to the viewport.
//
// Four floats and nothing else -- deliberately not `gfx::FloatRect`, which
// this module may not see. The type a geometry answer travels in is part of
// the seam rather than a convenience, and a seam that needed `gfx` would be
// one `allow:` line away from needing `layout`.
struct GeometryRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// The three boxes of the CSS box model a page can ask about, as values.
//
// A copy that outlives nothing. Script may hold one forever and it cannot
// become a dangling reference into a box tree a later layout rebuilt -- which
// is the whole security content of ADR 0015, because in a browser a
// use-after-free is an exploit primitive rather than a crash.
struct BoxGeometry {
  GeometryRect border_box;
  GeometryRect padding_box;
  GeometryRect content_box;

  // Where this box's content is displaced to, and how big that content is --
  // `scrollTop`/`scrollLeft` and `scrollWidth`/`scrollHeight`. Four numbers
  // rather than a fifth rectangle because they are not one: an offset is a
  // point and an overflow size is a size, and a rectangle made of the two would
  // invite reading a position out of a pair that has none.
  //
  // ADR 0018 §1. `scrollTop` is measured at 254 occurrences across the survey,
  // which is more than `getBoundingClientRect`, and a browser that answered
  // zero for all of them would run no virtualised list and restore no feed
  // position.
  float scroll_x = 0.0f;
  float scroll_y = 0.0f;
  float scroll_width = 0.0f;
  float scroll_height = 0.0f;
};

// Intrinsic dimensions of a decoded `<img>`, or nothing when the element is not
// an image or has no selected URL yet.
struct ImageState {
  int natural_width = 0;
  int natural_height = 0;
  bool complete = false;
};

// Where a geometry question is answered.
//
// Declared *here*, in the module that asks, and implemented by `src/engine`,
// which is the module that owns both a document and a layout. ADR 0015 sketched
// this the other way round -- a header the engine publishes -- but `src/bindings`
// may see only `util`, `js`, `dom` and `html`: not `engine`, not `layout`, not
// even `gfx`. That is ADR 0008's boundary, and inverting the dependency is what
// keeps it intact rather than widening `allow:` by one line and deleting the
// reason this module exists.
//
// Two rules hold this together and both are load-bearing:
//
//   * **Values out, never pointers.** Nothing here returns anything that points
//     into the box tree.
//   * **The answer is never stale.** An implementation that finds its layout
//     out of date runs it before answering. Returning last frame's rectangle is
//     a wrong answer, and refusing to answer is a wrong answer that also breaks
//     every framework.
class GeometrySource {
 public:
  virtual ~GeometrySource() = default;

  // The boxes of `node`, or nothing when it has none -- `display: none`, a
  // detached subtree, a node that is not an element. The caller turns that into
  // the all-zero rectangle the specification asks for, which is not a stub in
  // ADR 0012's sense: an element with no box genuinely has no geometry.
  virtual std::optional<BoxGeometry> QueryBox(const dom::Node& node) = 0;

  // The resolved value of one CSS property, serialized -- which for the
  // layout-dependent properties is the *used* value in pixels and for the rest
  // is the computed value. Nothing when this engine has no answer for the
  // property, which the caller reports as the empty string, exactly as the
  // specification says an unsupported property reads back.
  virtual std::optional<std::string> QueryUsedValue(const dom::Element& element,
                                                    std::string_view property) = 0;

  // Scrolls `node`'s box to (`x`, `y`), clamped by the implementation to what
  // that box can actually reach. `scrollTop = 1e9` is how a page scrolls a chat
  // log to the bottom, so a value out of range is ordinary rather than hostile
  // -- and clamping here rather than at the caller is what makes the two ways
  // to write it agree.
  //
  // A command rather than a question, and the only one on this interface. It
  // returns nothing because there is nothing useful to say: the value that took
  // effect is read back with QueryBox, through the same clamp.
  virtual void SetScrollOffset(const dom::Node& node, float x, float y) = 0;

  // The scrollport the document scrolls inside, in the coordinate system every
  // other answer here is measured in -- so its origin is (0, 0) by definition
  // and only the size carries information. A rectangle rather than a size
  // because that is what it *is* to the caller: `window.innerWidth`, and the
  // `rootBounds` an `IntersectionObserver` with no `root` intersects against.
  //
  // Not optional. A document always has a viewport, even when it is zero by
  // zero because nothing has been laid out yet -- and zero by zero is the
  // honest answer there rather than an absence, since nothing is on screen.
  virtual GeometryRect QueryViewport() = 0;

  // The device pixel ratio, *unquantised*. Quantising happens where the answer is handed to a page
  // (ADR 0029 §6, `bindings::QuantizeDevicePixelRatio`) rather than here, because this same number
  // selects an `<img srcset>` candidate -- and a page should be told a reduced ratio while the image
  // chosen for it is the one the real display wants. Two different consumers, one truth, one of them
  // reduced.
  virtual double QueryDevicePixelRatio() const = 0;

  // Whether a media query list matches right now.
  //
  // Here rather than in the binding layer because the evaluator lives in
  // `src/css`, which this module may not see -- the same inversion the rest of
  // this interface is, and the same reason. A media query is a pure function of
  // the environment, and the environment is what the engine has.
  //
  // It is on *this* interface rather than one of its own because the thing it
  // is a function of is the viewport: `matchMedia('(max-width: 700px)')` and
  // `window.innerWidth` are two spellings of one question, and answering them
  // through two seams is how they come to disagree.
  //
  // False for a query this evaluator does not implement, which is what the
  // specification says and is also the fingerprinting-safe answer: every media
  // feature is something this browser tells a page about the machine it is
  // running on. See ADR 0029.
  virtual bool QueryMediaMatches(std::string_view query) = 0;

  // Scrolls every scrolling ancestor of `node` -- not just the nearest one --
  // until `node` is inside each of their scrollports. `scrollIntoView` on an
  // item in a menu inside a scrolled page has to move both, and an
  // implementation that moved one is the one that looks right in a demo.
  virtual void ScrollIntoView(const dom::Node& node) = 0;

  // Whether an `<img>`'s selected URL has been decoded and its intrinsic size.
  // Nothing when the node is not an image or has no `src`/`srcset` selection.
  virtual std::optional<ImageState> QueryImageState(const dom::Element& element) = 0;
};

}  // namespace microbrowser::bindings
