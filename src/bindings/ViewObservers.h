#pragma once

#include <string_view>

#include "bindings/Geometry.h"

namespace microbrowser::bindings {

// The one piece of `IntersectionObserver` that is a *parser*.
//
// `rootMargin` is a string a page writes and this browser turns into four
// numbers that decide which images it fetches. Public, and with a fuzz target
// beside it, for that reason alone: everything else in ViewObservers.cpp reads
// numbers off an options object, and this reads text.
struct RootMargin {
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  float left = 0.0f;
};

// The CSS margin shorthand over `<length>` and `<percentage>`: one value is all
// four sides, two are vertical then horizontal, three are top, horizontal,
// bottom. Percentages resolve against `root` -- against its height for the
// vertical pair and its width for the horizontal one.
//
// Every component is finite and bounded, whatever the input says. That is not
// defensive tidiness: `rootMargin: '1e300px'` parses as a finite *double* and
// becomes an infinite float, and an infinite root makes every intersection
// ratio `inf/inf` -- a NaN that compares false against every threshold, so an
// observer would go silent rather than fire wrongly. Silence is the failure
// mode ADR 0018 §5 says must not ship.
//
// An unparseable component is zero rather than a throw. `rootMargin: '0'` is
// what half the pages that set one write, and refusing it would break them over
// a value that means nothing.
RootMargin ParseRootMargin(std::string_view text, const GeometryRect& root);

// `rect` grown by `margin` on each side. Separate from the parse so a caller
// can be given a margin from somewhere else -- and so the fuzz target can
// assert the result is a rectangle rather than an infinity.
GeometryRect ExpandedBy(const GeometryRect& rect, const RootMargin& margin);

}  // namespace microbrowser::bindings
