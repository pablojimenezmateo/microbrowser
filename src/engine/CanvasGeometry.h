#pragma once

#include <vector>

#include "gfx/AffineTransform.h"
#include "gfx/Path.h"

namespace microbrowser::engine {

// The path constructions the canvas API has and `gfx::Path` deliberately does not.
//
// `gfx::Path`'s own header states the rule: its verb set is "exactly the set every 2D path format
// reduces to", and arcs are built from cubics by the code that wants them. This is that code. It is
// its own translation unit rather than a corner of `CanvasSurfaces.cpp` because it is pure geometry
// -- no surface, no state, no element -- and that is what makes it testable without a document.
//
// Every function takes the current transform and applies it *per point*, which is what a canvas
// arc actually is: `arc()` under a non-uniform scale is an ellipse, and flattening in user space and
// then transforming the polyline is the only construction that gets that right.

// A circular or elliptical arc, appended to `path`.
//
// `have_current` is in-out: an arc with an open subpath begins with a line to its start point (the
// specification's rule, and why `arc` after a `moveTo` elsewhere draws a connector), and one without
// begins with a move.
void AppendEllipseArc(gfx::Path& path, const gfx::AffineTransform& transform, double cx, double cy,
                      double radius_x, double radius_y, double rotation, double start, double end,
                      bool counter_clockwise, bool& have_current);

// `arcTo`'s tangent construction: the arc of radius `radius` tangent to both the line from the
// current point to (x1, y1) and the line from (x1, y1) to (x2, y2).
//
// Takes the *untransformed* current point, because the construction is defined in user space and the
// path holds device-space points -- so the caller keeps the user-space pen position.
void AppendArcTo(gfx::Path& path, const gfx::AffineTransform& transform, double from_x,
                 double from_y, double x1, double y1, double x2, double y2, double radius);

// `roundRect`, with the specification's already-normalised per-corner (x, y) radii in the order
// top-left, top-right, bottom-right, bottom-left. Negative width or height flips the rectangle,
// which the specification requires and which is why this takes them signed.
void AppendRoundRect(gfx::Path& path, const gfx::AffineTransform& transform, double x, double y,
                     double width, double height, const std::vector<double>& radii);

// Whether (x, y) -- in device space, the space the path is in -- is inside `path` under `even_odd`.
//
// A crossing count over the flattened path rather than a rasterization, because the question is about
// one point: rasterizing a path to answer it would allocate coverage for its whole bounding box, and
// `isPointInPath` in a hit-testing loop is called once per shape per pointer move.
bool PathContainsPoint(const gfx::Path& path, double x, double y, bool even_odd);

// `path` with `transform` applied to every point.
//
// `gfx::Path` has no transform verb -- a path is geometry and a transform is a caller's business --
// so the walk is here. The canvas needs it in both directions: a stroke is widened in *user* space
// and painted in device space, so the current path goes back through the inverse and the resulting
// outline comes forward again.
gfx::Path Transformed(const gfx::Path& path, const gfx::AffineTransform& transform);

}  // namespace microbrowser::engine
