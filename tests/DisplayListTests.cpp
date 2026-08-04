#include <variant>
#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "gfx/Painter.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::DisplayList;
using gfx::IntRect;

void RegisterDisplayListTests(std::vector<TestCase>& tests) {
  AddTest(tests, "DisplayList/DegenerateCommandsAreNotRecorded", [] {
    DisplayList list;
    list.FillRect(IntRect{}, Color::Rgb(0xFF, 0, 0));
    list.FillRect(IntRect{0, 0, 10, 10}, Color::Rgba(0xFF, 0, 0, 0));
    Expect(list.IsEmpty(),
           "an empty rect or zero-alpha fill is a no-op; recording it makes every later "
           "frame diff carry it");
  });

  AddTest(tests, "DisplayList/EqualityIsStructural", [] {
    DisplayList a;
    DisplayList b;
    a.FillRect(IntRect{0, 0, 4, 4}, Color::Rgb(1, 2, 3));
    b.FillRect(IntRect{0, 0, 4, 4}, Color::Rgb(1, 2, 3));
    Expect(a == b, "identical lists must compare equal, which is what makes frame diffing cheap");

    b.FillRect(IntRect{0, 0, 4, 4}, Color::Rgb(1, 2, 4));
    Expect(!(a == b), "a differing color must make the lists unequal");
  });

  AddTest(tests, "DisplayList/ClearKeepsCapacity", [] {
    DisplayList list;
    for (int i = 0; i < 100; ++i) {
      list.FillRect(IntRect{i, 0, 1, 1}, Color::Rgb(0, 0, 0));
    }
    list.Clear();
    Expect(list.IsEmpty(), "Clear must empty the list");
  });

  AddTest(tests, "DisplayList/BoundsCoversEveryFill", [] {
    DisplayList list;
    list.FillRect(IntRect{10, 10, 5, 5}, Color::Rgb(0, 0, 0));
    list.FillRect(IntRect{100, 0, 5, 5}, Color::Rgb(0, 0, 0));
    Expect(list.Bounds() == (IntRect{10, 0, 95, 15}), "bounds must be the union of all fills");
  });

  AddTest(tests, "DisplayList/ExecuteRespectsDamage", [] {
    Canvas canvas(10, 10);
    canvas.Clear(Color::Rgb(0, 0, 0));

    DisplayList list;
    list.FillRect(canvas.Bounds(), Color::Rgb(0xFF, 0, 0));

    gfx::Painter painter(canvas);
    gfx::Execute(list, painter, IntRect{0, 0, 5, 10});
    ExpectEqInt(canvas.Row(0)[4], 0xFFFF0000, "inside the damage rect must be painted");
    ExpectEqInt(canvas.Row(0)[5], 0xFF000000,
                "outside the damage rect must be untouched; otherwise a partial repaint is "
                "no cheaper than a full one");
  });

  AddTest(tests, "DisplayList/ExecuteRestoresClipDepth", [] {
    Canvas canvas(10, 10);
    DisplayList list;
    // Deliberately unbalanced: three pushes, no pops.
    list.PushClip(IntRect{0, 0, 5, 5});
    list.PushClip(IntRect{0, 0, 4, 4});
    list.PushClip(IntRect{0, 0, 3, 3});

    const std::size_t before = canvas.ClipDepth();
    gfx::Painter painter(canvas);
    gfx::Execute(list, painter, canvas.Bounds());
    ExpectEqInt(static_cast<long long>(canvas.ClipDepth()), static_cast<long long>(before),
                "a malformed display list must not leak clip state into the next frame");
  });

  AddTest(tests, "DisplayList/ExecuteCannotPopPastItsDamageClip", [] {
    Canvas canvas(10, 10);
    canvas.Clear(Color::Rgb(0, 0, 0));

    DisplayList list;
    // An over-popping list must not be able to widen the clip past the damage
    // region and paint outside it.
    list.PopClip();
    list.PopClip();
    list.FillRect(canvas.Bounds(), Color::Rgb(0xFF, 0, 0));

    gfx::Painter painter(canvas);
    gfx::Execute(list, painter, IntRect{0, 0, 2, 2});
    ExpectEqInt(canvas.Row(0)[1], 0xFFFF0000, "inside the damage rect must be painted");
    ExpectEqInt(canvas.Row(0)[9], 0xFF000000,
                "an over-popping list must not escape the damage clip");
  });

  AddTest(tests, "DisplayList/ExecuteWithEmptyDamageDoesNothing", [] {
    Canvas canvas(4, 4);
    canvas.Clear(Color::Rgb(0, 0, 0));
    DisplayList list;
    list.FillRect(canvas.Bounds(), Color::Rgb(0xFF, 0, 0));
    gfx::Painter painter(canvas);
    gfx::Execute(list, painter, IntRect{});
    ExpectEqInt(canvas.Row(0)[0], 0xFF000000, "empty damage must paint nothing");
  });

  // --- Path commands --------------------------------------------------------

  AddTest(tests, "DisplayList/PathCommandsCarryTheirGeometryInASideTable", [] {
    gfx::Path triangle;
    triangle.MoveTo(gfx::FloatPoint{0.0f, 0.0f});
    triangle.LineTo(gfx::FloatPoint{8.0f, 0.0f});
    triangle.LineTo(gfx::FloatPoint{0.0f, 8.0f});
    triangle.Close();

    DisplayList list;
    list.FillPath(triangle, Color::Rgb(1, 2, 3));
    list.StrokePath(triangle, gfx::StrokeStyle{}, Color::Rgb(4, 5, 6));

    ExpectEqInt(static_cast<long long>(list.Size()), 2, "two commands");
    ExpectEqInt(static_cast<long long>(list.Paths().size()), 2, "each names its own path");
    Expect(list.Paths()[0] == triangle, "the geometry survives unchanged");

    list.Clear();
    Expect(list.Paths().empty(), "Clear must drop the path table too, or it grows forever");
  });

  AddTest(tests, "DisplayList/DegeneratePathCommandsAreDropped", [] {
    DisplayList list;
    gfx::Path empty;
    list.FillPath(empty, Color::Rgb(0, 0, 0));

    gfx::Path square;
    square.AddRect(gfx::FloatRect{0.0f, 0.0f, 4.0f, 4.0f});
    list.FillPath(square, Color::Transparent());
    gfx::StrokeStyle zero;
    zero.width = 0.0f;
    list.StrokePath(square, zero, Color::Rgb(0, 0, 0));

    Expect(list.IsEmpty(), "invisible commands never enter the list");
    Expect(list.Paths().empty(), "and neither does their geometry");
  });

  AddTest(tests, "DisplayList/BoundsOutsetsAStrokeByWhatItCanReach", [] {
    gfx::Path line;
    line.MoveTo(gfx::FloatPoint{10.0f, 10.0f});
    line.LineTo(gfx::FloatPoint{20.0f, 10.0f});

    DisplayList fill_only;
    fill_only.FillPath(line, Color::Rgb(0, 0, 0));

    DisplayList stroked;
    gfx::StrokeStyle style;
    style.width = 8.0f;
    stroked.StrokePath(line, style, Color::Rgb(0, 0, 0));

    // Damage that under-covers a stroke leaves stale pixels on screen, so the
    // outset has to account for the widest thing a stroke can grow: a mitered
    // spike at the limit.
    Expect(stroked.Bounds().Top() <= fill_only.Bounds().Top() - 4,
           "the stroke's damage must reach at least half its width beyond the path");
    Expect(stroked.Bounds().Left() <= fill_only.Bounds().Left() - 4,
           "on every side");
  });

  AddTest(tests, "DisplayList/ExecuteDrawsPathsThroughThePainter", [] {
    Canvas canvas(16, 16);
    canvas.Clear(Color::Rgb(0, 0, 0));

    gfx::Path square;
    square.AddRect(gfx::FloatRect{4.0f, 4.0f, 8.0f, 8.0f});
    DisplayList list;
    list.FillPath(square, Color::Rgb(0xFF, 0, 0));

    gfx::Painter painter(canvas);
    gfx::Execute(list, painter, canvas.Bounds());
    ExpectEqInt(canvas.Row(8)[8], 0xFFFF0000, "the path interior is painted");
    ExpectEqInt(canvas.Row(0)[0], 0xFF000000, "and its exterior is not");
  });

  // Regression: the outset went through a raw static_cast<int>, so a stroke
  // width of 1e38 made `ceil(...)` infinite and the cast undefined behavior.
  // The width is a wire field, so "nobody would set that" is not an argument.
  AddTest(tests, "DisplayList/AnAbsurdStrokeWidthProducesABoundedDamageRect", [] {
    gfx::Path line;
    line.MoveTo(gfx::FloatPoint{0.0f, 0.0f});
    line.LineTo(gfx::FloatPoint{10.0f, 0.0f});

    for (const float width : {1e30f, 1e38f, 3.4e38f}) {
      DisplayList list;
      gfx::StrokeStyle style;
      style.width = width;
      style.miter_limit = 1e30f;
      list.StrokePath(line, style, Color::Rgb(0, 0, 0));
      const IntRect bounds = list.Bounds();
      Expect(gfx::IsWithinDeviceRange(bounds),
             "damage must stay inside the range that makes rect arithmetic total, however "
             "large the stroke claims to be");
    }
  });

  AddTest(tests, "DisplayList/AnIndexNoCommandProducedResolvesToNothing", [] {
    // The builder cannot emit a dangling index, but Execute and Bounds both
    // index a vector with whatever a command carries, so the range check lives
    // with the data. Untested, it would be a bounds check nobody has watched
    // work.
    DisplayList list;
    gfx::Path square;
    square.AddRect(gfx::FloatRect{0.0f, 0.0f, 8.0f, 8.0f});
    list.FillPath(square, Color::Rgb(0xFF, 0, 0));

    Expect(list.PathAt(0) != nullptr, "the path the builder recorded resolves");
    Expect(list.PathAt(1) == nullptr, "one past the end does not");
    Expect(list.PathAt(0xFFFFFFFFu) == nullptr, "and neither does an absurd index");
    Expect(DisplayList{}.PathAt(0) == nullptr, "an empty list resolves nothing at all");
  });

  // ADR 0013's surface, and the two properties that make it a surface rather
  // than a very large image: nothing about the pixels is recorded here, and the
  // command survives Execute untouched.
  AddTest(tests, "DisplayList/ASurfaceIsRecordedAsAHoleAndNotAsPixels", [] {
    DisplayList list;
    list.DrawSurface(7u, IntRect{10, 20, 320, 240});
    ExpectEqInt(static_cast<long long>(list.Size()), 1, "one command");
    const auto* hole = std::get_if<gfx::DrawSurfaceCommand>(&list.Commands()[0]);
    Expect(hole != nullptr, "and it is a surface hole");
    ExpectEqInt(static_cast<long long>(hole->surface), 7, "naming the surface it was given");
    Expect(hole->destination == IntRect{10, 20, 320, 240}, "at the geometry it was given");
    ExpectEqInt(static_cast<long long>(list.Images().size()), 0,
                "and no image was stored: a surface is named, never carried");
    Expect(list.Bounds() == IntRect{10, 20, 320, 240},
           "its rectangle is still part of what this list determines");
  });

  AddTest(tests, "DisplayList/ASurfaceHoleRecordsNothingWhenItNamesNothing", [] {
    DisplayList list;
    list.DrawSurface(gfx::kNoSurface, IntRect{0, 0, 8, 8});
    Expect(list.IsEmpty(), "surface zero is not a surface");
    list.DrawSurface(3u, IntRect{});
    Expect(list.IsEmpty(), "and an empty destination is not a placement");
  });

  // The clip is resolved by the producer of placements, because the consumer is
  // the presenter and it has the placements and not the list. A video that
  // ignored the clip around it would paint over the browser chrome above the
  // page it is in.
  AddTest(tests, "DisplayList/SurfacePlacementsCarryTheClipTheyWereRecordedUnder", [] {
    DisplayList list;
    list.PushClip(IntRect{0, 50, 200, 100});
    list.DrawSurface(2u, IntRect{0, 0, 200, 200});
    list.PopClip();
    list.DrawSurface(3u, IntRect{300, 0, 20, 20});

    const std::vector<gfx::SurfacePlacement> placements = gfx::SurfacePlacements(list);
    ExpectEqInt(static_cast<long long>(placements.size()), 2, "both holes are placements");
    Expect(placements[0].destination == IntRect{0, 50, 200, 100},
           "the clipped one is intersected with its clip");
    Expect(placements[1].destination == IntRect{300, 0, 20, 20},
           "and the unclipped one is not narrowed");
  });

  AddTest(tests, "DisplayList/ASurfaceClippedAwayIsNotAPlacement", [] {
    DisplayList list;
    list.PushClip(IntRect{0, 0, 10, 10});
    list.DrawSurface(2u, IntRect{500, 500, 20, 20});
    list.PopClip();
    Expect(gfx::SurfacePlacements(list).empty(),
           "a caller counting placements is counting things it has to composite");
  });
}

}  // namespace microbrowser::tests
