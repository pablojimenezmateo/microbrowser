#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.h"
#include "gfx/Path.h"
#include "gfx/SvgDecoder.h"
#include "gfx/SvgPath.h"

namespace microbrowser::tests {

using gfx::Color;
using gfx::DecodeSvg;
using gfx::Image;
using gfx::ParseSvgPathData;
using gfx::Path;

namespace {

constexpr std::size_t kPlentyOfCommands = 4096;

Path ParsePath(std::string_view data) {
  Path path;
  ParseSvgPathData(data, path, kPlentyOfCommands);
  return path;
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

Image Render(std::string_view document, int width = 0, int height = 0) {
  const std::vector<std::byte> bytes = Bytes(document);
  gfx::SvgDecodeResult result = DecodeSvg(bytes, width, height);
  return std::move(result.image);
}

Color At(const Image& image, int x, int y) {
  if (!image.IsValid() || x < 0 || y < 0 || x >= image.Width() || y >= image.Height()) {
    return Color::Transparent();
  }
  return Color{image.Row(y)[x]};
}

// The path's points, so a test can state where the pen went instead of
// asserting on a rendered pixel.
std::vector<gfx::FloatPoint> PointsOf(const Path& path) {
  return std::vector<gfx::FloatPoint>(path.Points().begin(), path.Points().end());
}

bool Near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }

}  // namespace

void RegisterSvgTests(std::vector<TestCase>& tests) {
  // --- Path data ------------------------------------------------------------

  AddTest(tests, "SvgPath/ACommandRepeatsWithoutBeingRepeated", [] {
    // `L 1 1 2 2` is two linetos. Every icon in the world relies on this and a
    // parser that reads one command per letter draws half of each outline.
    const std::vector<gfx::FloatPoint> points = PointsOf(ParsePath("M0 0 L1 1 2 2 3 3"));
    ExpectEqInt(static_cast<long long>(points.size()), 4, "a moveto and three linetos");
    Expect(Near(points.back().x, 3.0f) && Near(points.back().y, 3.0f), "ending at 3,3");
  });

  AddTest(tests, "SvgPath/ARepeatedMovetoBecomesALineto", [] {
    // What closes most outlines: `M0 0 1 1` is a moveto then a *lineto*, not
    // two movetos, so the shape has an edge instead of two empty contours.
    const Path repeated = ParsePath("M0 0 10 0 10 10 Z");
    const Path spelled = ParsePath("M0 0 L10 0 L10 10 Z");
    Expect(repeated == spelled, "a second coordinate pair after M is a lineto");
    Expect(ParsePath("m0 0 10 0") == ParsePath("m0 0 l10 0"),
           "and a relative moveto continues as a relative lineto");
  });

  AddTest(tests, "SvgPath/NumbersNeedNoSeparatorWhenTheSignSuppliesOne", [] {
    // `m4 4h188v188h-188z` is the Hacker News logo's outer square. A parser
    // that requires a separator reads `h-188` as a command it does not know.
    Expect(ParsePath("M1-2") == ParsePath("M1 -2"), "a minus sign separates two numbers");
    Expect(ParsePath("M.5.5") == ParsePath("M0.5 0.5"), "and so does a second decimal point");
    Expect(ParsePath("M1e2 0") == ParsePath("M100 0"),
           "but `1e2` is one number: the exponent's sign is part of it, not a subtraction");
    Expect(ParsePath("M1e 0") == ParsePath("M1"),
           "`1e` with no exponent ends the number, and `e` is not a command");
  });

  AddTest(tests, "SvgPath/HorizontalAndVerticalKeepTheOtherAxis", [] {
    const std::vector<gfx::FloatPoint> points = PointsOf(ParsePath("M4 4 h188 v188 h-188 Z"));
    ExpectEqInt(static_cast<long long>(points.size()), 4, "four points");
    Expect(Near(points[1].x, 192.0f) && Near(points[1].y, 4.0f), "h moves along x only");
    Expect(Near(points[2].x, 192.0f) && Near(points[2].y, 192.0f), "v moves along y only");
    Expect(Near(points[3].x, 4.0f) && Near(points[3].y, 192.0f), "and a negative h moves back");
  });

  AddTest(tests, "SvgPath/ClosepathReturnsThePenToTheSubpathStart", [] {
    // A relative command after Z is relative to where the subpath *began*, not
    // to where the pen had got to. Getting this wrong displaces every subpath
    // after the first, which on an icon means every hole in the shape.
    const std::vector<gfx::FloatPoint> points = PointsOf(ParsePath("M10 10 l5 0 Z l0 5"));
    Expect(!points.empty(), "the path has points");
    Expect(Near(points.back().x, 10.0f) && Near(points.back().y, 15.0f),
           "the lineto after Z starts from 10,10 rather than from 15,10");
  });

  AddTest(tests, "SvgPath/ShorthandCurvesReflectOnlyAMatchingPredecessor", [] {
    // `S` reflects the previous *cubic's* control point. After anything else
    // there is nothing to reflect, and the specification says the first control
    // point coincides with the current point.
    const Path after_cubic = ParsePath("M0 0 C1 1 2 2 3 3 S5 5 6 6");
    const Path unmatched = ParsePath("M0 0 L3 3 S5 5 6 6");
    Expect(!(after_cubic == unmatched),
           "a reflected control point is not the same curve as an unreflected one");
    const std::vector<gfx::FloatPoint> points = PointsOf(unmatched);
    Expect(points.size() >= 3 && Near(points[2].x, 3.0f) && Near(points[2].y, 3.0f),
           "with nothing to reflect, the first control point is the current point");
  });

  AddTest(tests, "SvgPath/AZeroRadiusArcIsALine", [] {
    Expect(ParsePath("M0 0 A0 0 0 0 0 10 10") == ParsePath("M0 0 L10 10"),
           "the specification says an arc with a zero radius is a straight line");
  });

  AddTest(tests, "SvgPath/ArcFlagsNeedNoSeparator", [] {
    // `a5 5 0 1150 0` is radii, rotation, two flags and a point, with nothing
    // between the flags. Real icon data is written this way by minifiers.
    Expect(ParsePath("M0 0 a5 5 0 1150 0") == ParsePath("M0 0 a5 5 0 1 1 50 0"),
           "the arc flags are single characters and take no separator");
  });

  AddTest(tests, "SvgPath/MalformedDataKeepsWhatItParsed", [] {
    // A truncated `d` should draw what it managed. A page with one typo in one
    // icon must not lose the icon.
    const Path partial = ParsePath("M0 0 L10 10 L");
    Expect(!partial.IsEmpty(), "the commands before the truncation survive");
    Expect(PointsOf(partial).size() == 2, "and nothing after it is invented");
  });

  AddTest(tests, "SvgPath/CommandCountIsBounded", [] {
    std::string data = "M0 0";
    for (int i = 0; i < 500; ++i) {
      data += " l1 1";
    }
    Path path;
    Expect(!ParseSvgPathData(data, path, 16),
           "path data is attacker-controlled; a megabyte of `l1 1` is a megabyte of segments");
    Expect(path.VerbCount() <= 16, "and nothing past the bound was built");
  });

  // --- Documents ------------------------------------------------------------

  AddTest(tests, "Svg/RendersAViewBoxScaledToTheRequestedSize", [] {
    // The document is 4 units wide and asked for at 40 pixels, so the left half
    // is red and the right half is blue whatever the units were.
    const Image image = Render(
        R"SVG(<svg viewBox="0 0 4 4"><rect width="2" height="4" fill="#ff0000"/>)SVG"
        R"SVG(<rect x="2" width="2" height="4" fill="#0000ff"/></svg>)SVG",
        40, 40);
    Expect(image.IsValid() && image.Width() == 40 && image.Height() == 40,
           "rendered at the size the caller asked for, not the document's units");
    Expect(At(image, 10, 20) == Color::Rgb(0xFF, 0, 0), "the left half is the first rect");
    Expect(At(image, 30, 20) == Color::Rgb(0, 0, 0xFF), "and the right half the second");
  });

  AddTest(tests, "Svg/PreservesTheAspectRatioUnlessToldNotTo", [] {
    // `preserveAspectRatio` defaults to `xMidYMid meet`: scale uniformly and
    // centre the leftover. A renderer that stretched instead would distort
    // every icon whose viewBox is not the shape of its box -- silently, since a
    // stretched triangle is still a triangle.
    const Image fitted =
        Render(R"SVG(<svg viewBox="0 0 2 1"><rect width="2" height="1"/></svg>)SVG", 20, 20);
    Expect(At(fitted, 10, 10) == Color::Rgb(0, 0, 0), "the shape is drawn in the middle");
    Expect(At(fitted, 10, 1).IsFullyTransparent() && At(fitted, 10, 18).IsFullyTransparent(),
           "with the leftover split above and below it rather than stretched into");

    const Image stretched = Render(
        R"SVG(<svg viewBox="0 0 2 1" preserveAspectRatio="none"><rect width="2" height="1"/>)SVG"
        R"SVG(</svg>)SVG",
        20, 20);
    Expect(At(stretched, 10, 1) == Color::Rgb(0, 0, 0),
           "and only an explicit `none` fills the box");
  });

  AddTest(tests, "Svg/FallsBackToTheDocumentsOwnSize", [] {
    const Image image = Render(R"SVG(<svg width="12" height="7"><rect width="12" height="7"/></svg>)SVG");
    Expect(image.IsValid() && image.Width() == 12 && image.Height() == 7,
           "with no size requested the document's own width and height are used");
  });

  AddTest(tests, "Svg/FillDefaultsToBlackAndNoneMeansNone", [] {
    const Image filled = Render(R"SVG(<svg viewBox="0 0 2 2"><rect width="2" height="2"/></svg>)SVG",
                                10, 10);
    Expect(At(filled, 5, 5) == Color::Rgb(0, 0, 0), "an unstated fill is black");
    const Image empty =
        Render(R"SVG(<svg viewBox="0 0 2 2"><rect width="2" height="2" fill="none"/></svg>)SVG", 10, 10);
    Expect(At(empty, 5, 5).IsFullyTransparent(),
           "`fill=none` paints nothing, which is different from an unstated fill");
  });

  AddTest(tests, "Svg/PresentationInheritsThroughGroups", [] {
    const Image image = Render(
        R"SVG(<svg viewBox="0 0 4 2"><g fill="#00ff00"><rect width="2" height="2"/>)SVG"
        R"SVG(<rect x="2" width="2" height="2" fill="#ff0000"/></g></svg>)SVG",
        40, 20);
    Expect(At(image, 10, 10) == Color::Rgb(0, 0xFF, 0), "the group's fill reaches its child");
    Expect(At(image, 30, 10) == Color::Rgb(0xFF, 0, 0), "and a child's own fill still wins");
  });

  AddTest(tests, "Svg/AStyleAttributeBeatsAPresentationAttribute", [] {
    const Image image = Render(
        R"SVG(<svg viewBox="0 0 2 2"><rect width="2" height="2" fill="#ff0000")SVG"
        R"SVG( style="fill:#00ff00"/></svg>)SVG",
        10, 10);
    Expect(At(image, 5, 5) == Color::Rgb(0, 0xFF, 0),
           "SVG gives the style attribute precedence over the presentation attribute");
  });

  AddTest(tests, "Svg/TransformsApplyAndCompose", [] {
    const Image image = Render(
        R"SVG(<svg viewBox="0 0 4 4"><g transform="translate(2 0)">)SVG"
        R"SVG(<rect width="2" height="4" fill="#ff0000"/></g></svg>)SVG",
        40, 40);
    Expect(At(image, 10, 20).IsFullyTransparent(), "the rect moved off the left half");
    Expect(At(image, 30, 20) == Color::Rgb(0xFF, 0, 0), "and onto the right");
  });

  AddTest(tests, "Svg/AGradientReferenceIsNotApproximated", [] {
    // A flat colour is not a near miss for a gradient. Painting one would make
    // a logo look wrong in a way nobody would trace back to here.
    const Image image =
        Render(R"SVG(<svg viewBox="0 0 2 2"><rect width="2" height="2" fill="url(#g)"/></svg>)SVG",
               10, 10);
    Expect(image.IsValid(), "the document still renders");
    Expect(At(image, 5, 5).IsFullyTransparent(), "but the shape is left unpainted");
  });

  AddTest(tests, "Svg/RejectsWhatIsNotAnSvg", [] {
    const std::vector<std::byte> html = Bytes("<html><body>not a picture</body></html>");
    Expect(!DecodeSvg(html, 10, 10).Ok(), "a document with no <svg> element is not an image");
    Expect(!DecodeSvg({}, 10, 10).Ok(), "and neither is nothing at all");
    Expect(!DecodeSvg(Bytes("<svg></svg>"), 0, 0).Ok(),
           "an svg with no size anywhere -- no request, no width, no viewBox -- has none");
  });

  AddTest(tests, "Svg/SniffsTheBytesRatherThanTrustingAHeader", [] {
    Expect(gfx::LooksLikeSvg(Bytes(R"SVG(<?xml version="1.0"?><svg width="1" height="1"/>)SVG")),
           "an XML declaration before the root is normal");
    Expect(gfx::LooksLikeSvg(Bytes("<!-- a comment --><svg:svg/>")),
           "and so are a comment and a namespace prefix");
    Expect(!gfx::LooksLikeSvg(Bytes("<html><svgnot/></html>")),
           "`<svgnot` is not `<svg`");
    Expect(!gfx::LooksLikeSvg(Bytes(std::string(2000, 'x') + "<svg/>")),
           "a root past the sniff window is not found, because an unbounded search over a "
           "hostile blob is a denial of service dressed as a content sniff");
  });

  AddTest(tests, "Svg/BoundsTheOutputSize", [] {
    Expect(!DecodeSvg(Bytes("<svg/>"), gfx::kMaxSvgEdge + 1, 10).Ok(),
           "a size past the bound is refused before a surface is allocated");
  });

  AddTest(tests, "Svg/BoundsNestingAndElementCount", [] {
    // Neither of these is a crash today, and that is the point: the bounds are
    // what keep it that way as the supported subset grows.
    std::string deep = R"SVG(<svg viewBox="0 0 2 2">)SVG";
    for (std::size_t i = 0; i < gfx::kMaxSvgDepth * 4; ++i) {
      deep += "<g>";
    }
    deep += R"SVG(<rect width="2" height="2"/>)SVG";
    for (std::size_t i = 0; i < gfx::kMaxSvgDepth * 4; ++i) {
      deep += "</g>";
    }
    deep += "</svg>";
    Expect(DecodeSvg(Bytes(deep), 10, 10).Ok(), "a document nested past the bound still renders");

    std::string many = R"SVG(<svg viewBox="0 0 2 2">)SVG";
    for (std::size_t i = 0; i < gfx::kMaxSvgElements + 100; ++i) {
      many += R"SVG(<rect width="1" height="1"/>)SVG";
    }
    many += "</svg>";
    Expect(DecodeSvg(Bytes(many), 10, 10).Ok(), "and so does one with more elements than allowed");
  });
}

}  // namespace microbrowser::tests
