#include <vector>

#include "TestSupport.h"
#include "gfx/Canvas.h"
#include "gfx/DisplayList.h"
#include "support/ReferenceImage.h"

namespace microbrowser::tests {

using gfx::Canvas;
using gfx::Color;
using gfx::DisplayList;
using gfx::IntRect;

void RegisterReferenceImageTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ReferenceImage/EncodesAValidPpmHeader", [] {
    Canvas canvas(3, 2);
    canvas.Clear(Color::Rgb(0x10, 0x20, 0x30));
    const std::string ppm = EncodePpm(canvas);
    Expect(ppm.rfind("P6\n3 2\n255\n", 0) == 0, "PPM header must be P6 with the canvas size");
    ExpectEqInt(static_cast<long long>(ppm.size()), 11 + 3 * 2 * 3,
                "payload must be exactly width * height * 3 bytes");
  });

  AddTest(tests, "ReferenceImage/IdenticalCanvasesCompareEqual", [] {
    Canvas a(4, 4);
    Canvas b(4, 4);
    a.Clear(Color::Rgb(1, 2, 3));
    b.Clear(Color::Rgb(1, 2, 3));
    Expect(ComparePpm(EncodePpm(a), EncodePpm(b)).matches, "identical renders must compare equal");
  });

  AddTest(tests, "ReferenceImage/ReportsTheFirstDifferingPixel", [] {
    Canvas a(4, 4);
    Canvas b(4, 4);
    a.Clear(Color::Rgb(0, 0, 0));
    b.Clear(Color::Rgb(0, 0, 0));
    b.FillRect(IntRect{2, 1, 1, 1}, Color::Rgb(0xFF, 0, 0));

    const ComparisonResult result = ComparePpm(EncodePpm(a), EncodePpm(b));
    Expect(!result.matches, "a differing pixel must be detected");
    ExpectEqInt(static_cast<long long>(result.differing_pixels), 1, "exactly one pixel differs");
    ExpectEqInt(result.first_x, 2, "x of the first difference");
    ExpectEqInt(result.first_y, 1, "y of the first difference");
  });

  AddTest(tests, "ReferenceImage/SizeMismatchIsReportedNotCompared", [] {
    Canvas a(4, 4);
    Canvas b(5, 4);
    const ComparisonResult result = ComparePpm(EncodePpm(a), EncodePpm(b));
    Expect(!result.matches, "differently sized images cannot match");
    Expect(result.message.find("size mismatch") != std::string::npos,
           "the message must say the sizes differ rather than counting pixels");
  });

  AddTest(tests, "ReferenceImage/RejectsMalformedInput", [] {
    Canvas a(2, 2);
    Expect(!ComparePpm(EncodePpm(a), "not a ppm at all").matches,
           "a malformed golden must be reported, not silently treated as a mismatch of zero");
  });

  // The end-to-end shape a real reference test will have, run against an
  // in-memory expectation rather than a golden file so it needs no checked-in
  // binary at M0. Golden-backed cases arrive with the rasterizer in M1.
  AddTest(tests, "ReferenceImage/DisplayListRendersDeterministically", [] {
    const auto render = [] {
      Canvas canvas(16, 16);
      canvas.Clear(Color::Rgb(0xFF, 0xFF, 0xFF));
      DisplayList list;
      list.FillRect(IntRect{0, 0, 16, 4}, Color::Rgb(0x1F, 0x6F, 0xEB));
      list.PushClip(IntRect{2, 6, 12, 8});
      list.FillRect(IntRect{0, 6, 16, 3}, Color::Rgba(0x20, 0x20, 0x28, 0x80));
      list.PopClip();
      gfx::Execute(list, canvas, canvas.Bounds());
      return EncodePpm(canvas);
    };

    // Determinism is what makes golden files meaningful; without it the whole
    // reference-test strategy collapses.
    Expect(ComparePpm(render(), render()).matches,
           "the software rasterizer must produce byte-identical output for identical input");
  });
}

}  // namespace microbrowser::tests
