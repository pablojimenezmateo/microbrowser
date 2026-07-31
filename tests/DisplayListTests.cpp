#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"

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

    gfx::Execute(list, canvas, IntRect{0, 0, 5, 10});
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
    gfx::Execute(list, canvas, canvas.Bounds());
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

    gfx::Execute(list, canvas, IntRect{0, 0, 2, 2});
    ExpectEqInt(canvas.Row(0)[1], 0xFFFF0000, "inside the damage rect must be painted");
    ExpectEqInt(canvas.Row(0)[9], 0xFF000000,
                "an over-popping list must not escape the damage clip");
  });

  AddTest(tests, "DisplayList/ExecuteWithEmptyDamageDoesNothing", [] {
    Canvas canvas(4, 4);
    canvas.Clear(Color::Rgb(0, 0, 0));
    DisplayList list;
    list.FillRect(canvas.Bounds(), Color::Rgb(0xFF, 0, 0));
    gfx::Execute(list, canvas, IntRect{});
    ExpectEqInt(canvas.Row(0)[0], 0xFF000000, "empty damage must paint nothing");
  });
}

}  // namespace microbrowser::tests
