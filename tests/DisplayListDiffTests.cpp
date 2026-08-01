#include <algorithm>
#include <span>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayListDiff.h"
#include "gfx/Painter.h"

namespace microbrowser::tests {

using gfx::Color;
using gfx::DirtyRegion;
using gfx::DisplayList;
using gfx::IntRect;

namespace {

constexpr IntRect kViewport{0, 0, 200, 200};

gfx::Path Rect(float x, float y, float w, float h) {
  gfx::Path path;
  path.AddRect(gfx::FloatRect{x, y, w, h});
  return path;
}

// Renders a list and returns the canvas, so a test can ask the question that
// actually matters: does repainting only the damaged rects give the same
// pixels as repainting everything?
gfx::Canvas Render(const DisplayList& list, const std::vector<IntRect>& regions) {
  gfx::Canvas canvas(kViewport.width, kViewport.height);
  canvas.Clear(Color::Rgb(0, 0, 0));
  gfx::Painter painter(canvas);
  for (const IntRect& region : regions) {
    gfx::Execute(list, painter, region);
  }
  return canvas;
}

bool SamePixels(const gfx::Canvas& a, const gfx::Canvas& b) {
  if (a.Width() != b.Width() || a.Height() != b.Height()) {
    return false;
  }
  const std::span<const std::uint32_t> left = a.Pixels();
  const std::span<const std::uint32_t> right = b.Pixels();
  return std::equal(left.begin(), left.end(), right.begin());
}

// The property the whole diff exists to have: starting from the previous
// frame's pixels and repainting only the damage must land on the same image as
// repainting everything. Anything less leaves stale pixels on screen.
void ExpectDamageIsSufficient(const DisplayList& before, const DisplayList& after,
                              const char* label) {
  DirtyRegion damage;
  const bool bounded = gfx::ComputeDamage(before, after, kViewport, damage);

  gfx::Canvas incremental(kViewport.width, kViewport.height);
  incremental.Clear(Color::Rgb(0, 0, 0));
  gfx::Painter painter(incremental);
  gfx::Execute(before, painter, kViewport);
  // The damaged rects are repainted from scratch, which means clearing them
  // first -- exactly what a compositor does with a damage rect.
  for (const IntRect& rect : bounded ? damage.Rects() : std::vector<IntRect>{kViewport}) {
    incremental.FillRect(rect, Color::Rgb(0, 0, 0));
    gfx::Execute(after, painter, rect);
  }

  const gfx::Canvas full = Render(after, {kViewport});
  Expect(SamePixels(incremental, full), label);
}

}  // namespace

void RegisterDisplayListDiffTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Damage/IdenticalListsProduceNoDamage", [] {
    DisplayList list;
    list.FillRect(IntRect{10, 10, 20, 20}, Color::Rgb(1, 2, 3));
    list.FillPath(Rect(50.0f, 50.0f, 10.0f, 10.0f), Color::Rgb(4, 5, 6));

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(list, list, kViewport, damage), "the diff was bounded");
    Expect(damage.IsEmpty(),
           "a frame that draws the same picture must not be repainted, which is most of what "
           "a browser wastes power on");
  });

  AddTest(tests, "Damage/AChangedCommandDamagesOnlyItself", [] {
    DisplayList before;
    before.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0xFF, 0xFF));
    before.FillRect(IntRect{10, 10, 20, 20}, Color::Rgb(0xFF, 0, 0));
    before.FillRect(IntRect{150, 150, 20, 20}, Color::Rgb(0, 0xFF, 0));

    DisplayList after;
    after.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0xFF, 0xFF));
    after.FillRect(IntRect{10, 10, 20, 20}, Color::Rgb(0, 0, 0xFF));  // recoloured
    after.FillRect(IntRect{150, 150, 20, 20}, Color::Rgb(0, 0xFF, 0));

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(!damage.IsEmpty(), "something changed");
    Expect(damage.BoundingBox().width <= 20 && damage.BoundingBox().height <= 20,
           "the unchanged background and the unchanged far corner are not repainted");
    ExpectDamageIsSufficient(before, after, "a recoloured rect");
  });

  AddTest(tests, "Damage/AnInsertedCommandDoesNotDirtyTheWholeTail", [] {
    // Matching from both ends is what makes this work: an insertion shifts
    // every later command's index, and a prefix-only diff would mark all of
    // them changed.
    DisplayList before;
    for (int i = 0; i < 6; ++i) {
      before.FillRect(IntRect{i * 30, 0, 20, 20}, Color::Rgb(1, 1, 1));
    }
    DisplayList after;
    for (int i = 0; i < 6; ++i) {
      if (i == 3) {
        after.FillRect(IntRect{100, 100, 5, 5}, Color::Rgb(9, 9, 9));
      }
      after.FillRect(IntRect{i * 30, 0, 20, 20}, Color::Rgb(1, 1, 1));
    }

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(damage.BoundingBox().width < kViewport.width,
           "an insertion damages the inserted command, not everything after it");
    ExpectDamageIsSufficient(before, after, "an inserted command");
  });

  AddTest(tests, "Damage/TwoCommandsWithTheSamePathIndexAreNotAssumedEqual", [] {
    // A FillPathCommand holds an index into its own list's path table. Two
    // lists can use index 0 for entirely different geometry, and comparing the
    // commands alone would call a circle and a square equal -- then skip
    // repainting the change.
    DisplayList before;
    before.FillPath(Rect(0.0f, 0.0f, 10.0f, 10.0f), Color::Rgb(0xFF, 0, 0));
    DisplayList after;
    after.FillPath(Rect(100.0f, 100.0f, 50.0f, 50.0f), Color::Rgb(0xFF, 0, 0));

    Expect(!gfx::CommandsPaintTheSame(before, before.Commands().at(0), after,
                                      after.Commands().at(0)),
           "same index, different geometry, and the diff must notice");

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(damage.BoundingBox().Right() >= 150, "the new shape's area is damaged");
    ExpectDamageIsSufficient(before, after, "a path that moved");
  });

  AddTest(tests, "Damage/TwoCommandsWithTheSameTextIndexAreNotAssumedEqual", [] {
    DisplayList before;
    before.DrawText("one", 30.0f, gfx::FontRequest{"Test", 16.0f, 400, false},
                    gfx::FloatPoint{10.0f, 20.0f}, Color::Rgb(0, 0, 0));
    DisplayList after;
    after.DrawText("two", 30.0f, gfx::FontRequest{"Test", 16.0f, 400, false},
                   gfx::FloatPoint{10.0f, 20.0f}, Color::Rgb(0, 0, 0));

    Expect(!gfx::CommandsPaintTheSame(before, before.Commands().at(0), after,
                                      after.Commands().at(0)),
           "different text at the same index is a difference");

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(!damage.IsEmpty(), "and the run's area is damaged");
  });

  AddTest(tests, "Damage/AChangedClipFallsBackToAFullRepaint", [] {
    // A clip is state every later command reads, so an identical command after
    // a changed clip draws somewhere else. Modelling that is the point at
    // which a diff stops being a diff and becomes a second renderer.
    DisplayList before;
    before.PushClip(IntRect{0, 0, 50, 50});
    before.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0, 0));
    before.PopClip();

    DisplayList after;
    after.PushClip(IntRect{0, 0, 150, 150});
    after.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0, 0));
    after.PopClip();

    DirtyRegion damage;
    Expect(!gfx::ComputeDamage(before, after, kViewport, damage),
           "the diff reports that it could not bound the change");
    ExpectEqInt(static_cast<long long>(damage.Rects().size()), 1, "and asks for one rect");
    Expect(damage.Rects().at(0) == kViewport, "covering everything");
    ExpectDamageIsSufficient(before, after, "a moved clip");
  });

  AddTest(tests, "Damage/AnEmptyPreviousFrameDamagesWhatIsDrawn", [] {
    const DisplayList before;
    DisplayList after;
    after.FillRect(IntRect{20, 20, 40, 40}, Color::Rgb(1, 2, 3));

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(damage.BoundingBox() == IntRect{20, 20, 40, 40},
           "the first frame damages exactly what it paints, not the whole surface");
  });

  AddTest(tests, "Damage/RemovingACommandDamagesWhereItWas", [] {
    // The half that is easy to forget: a command that vanished leaves its
    // pixels behind unless the region it used to cover is repainted.
    DisplayList before;
    before.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0xFF, 0xFF));
    before.FillRect(IntRect{60, 60, 30, 30}, Color::Rgb(0xFF, 0, 0));
    DisplayList after;
    after.FillRect(IntRect{0, 0, 200, 200}, Color::Rgb(0xFF, 0xFF, 0xFF));

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(damage.BoundingBox() == IntRect{60, 60, 30, 30},
           "the departed rect's own area is what needs repainting");
    ExpectDamageIsSufficient(before, after, "a removed command");
  });

  AddTest(tests, "Damage/IsClippedToTheSurface", [] {
    DisplayList before;
    DisplayList after;
    after.FillRect(IntRect{-500, -500, 2000, 2000}, Color::Rgb(1, 1, 1));

    DirtyRegion damage;
    Expect(gfx::ComputeDamage(before, after, kViewport, damage), "bounded");
    Expect(damage.BoundingBox() == kViewport,
           "damage outside the surface is not damage; a rect larger than the window would "
           "make every later intersection do the clamping instead");
  });
}

}  // namespace microbrowser::tests
