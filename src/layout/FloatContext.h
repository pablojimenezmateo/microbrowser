#pragma once

#include <vector>

#include "css/ComputedStyle.h"
#include "gfx/Geometry.h"

namespace microbrowser::layout {

// The floats placed in one block formatting context.
//
// A float is the one thing in CSS block layout that a box's own geometry
// cannot express: it is taken out of the flow, and then every line box *after*
// it is shortened around it, including lines belonging to boxes that know
// nothing about it. That shared, order-dependent state is what this type is —
// separate from Box because it belongs to the formatting context rather than
// to any box in it, and separate from LayoutEngine so the placement rules can
// be tested without a document.
//
// Coordinates are absolute, the same space Box geometry is in.
class FloatContext {
 public:
  void Reset() { floats_.clear(); }
  bool IsEmpty() const { return floats_.empty(); }
  std::size_t Count() const { return floats_.size(); }

  // The horizontal band available at `y` for a box `height` tall, within the
  // containing block's [left, right].
  //
  // Takes a height rather than a point because a line box is not a line: a tall
  // line clearing the bottom of a float at its top would still overlap it lower
  // down. Only floats that overlap the vertical range narrow the band.
  struct Band {
    float left = 0.0f;
    float right = 0.0f;

    float Width() const { return right - left; }
  };
  Band BandAt(float y, float height, float left, float right) const;

  // Places a float `width` x `height` no higher than `y`, and returns where it
  // landed. Moves down past floats it cannot fit beside, which is the whole of
  // the placement rule that matters: CSS 2.1 §9.5.1 says a float's top may not
  // be higher than the top of any earlier float, and that it must not overlap
  // one.
  gfx::FloatRect Place(css::Float side, float width, float height, float y, float left,
                       float right);

  // The lowest edge a box must start at to clear the named floats, or `y` when
  // there is nothing to clear.
  float ClearanceBelow(css::Clear which, float y) const;

  // The bottom of the lowest float. Used by a formatting context that has to
  // contain its floats, where the parent's height is the lower of its content
  // and this.
  float LowestBottom() const;

 private:
  struct Placed {
    gfx::FloatRect rect;
    css::Float side = css::Float::Left;
  };

  std::vector<Placed> floats_;
};

}  // namespace microbrowser::layout
