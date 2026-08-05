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
};

}  // namespace microbrowser::bindings
