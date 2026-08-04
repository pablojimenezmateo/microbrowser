#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/JpegDecoder.h"
#include "support/JpegFixtures.h"

namespace microbrowser::tests {

using gfx::DecodeJpeg;
using gfx::Image;
using gfx::JpegDecodeResult;

namespace {

struct Difference {
  int worst = 0;
  double mean = 0.0;
};

// How far this decoder is from libjpeg-turbo on the same file.
//
// Two conforming IDCTs disagree; T.83 defines conformance as a bound on that
// disagreement rather than as equality, so the test states a bound too. What it
// must catch is the failure that actually happens — a shifted row, a swapped
// chroma plane, a missed successive-approximation bit — and every one of those
// moves the mean by tens of levels, not by one.
Difference CompareToReference(const Image& image, const JpegFixture& fixture) {
  Difference difference;
  double total = 0.0;
  for (int y = 0; y < fixture.height; ++y) {
    const std::uint32_t* row = image.Row(y);
    for (int x = 0; x < fixture.width; ++x) {
      const std::size_t index =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(fixture.width) +
           static_cast<std::size_t>(x)) *
          3u;
      const std::uint32_t pixel = row[x];
      const std::array<int, 3> ours = {static_cast<int>((pixel >> 16) & 0xFF),
                                       static_cast<int>((pixel >> 8) & 0xFF),
                                       static_cast<int>(pixel & 0xFF)};
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const int theirs = fixture.rgb[index + channel];
        const int delta = std::abs(ours[channel] - theirs);
        difference.worst = std::max(difference.worst, delta);
        total += delta;
      }
    }
  }
  const auto samples =
      static_cast<double>(fixture.width) * static_cast<double>(fixture.height) * 3.0;
  difference.mean = total / samples;
  return difference;
}

void ExpectMatchesReference(const JpegFixture& fixture, std::string_view what, int worst_allowed,
                            double mean_allowed) {
  const JpegDecodeResult result = DecodeJpeg(fixture.bytes);
  Expect(result.Ok(), result.error != nullptr ? result.error : "decode failed");
  ExpectEqInt(result.image.Width(), fixture.width, "width from SOF");
  ExpectEqInt(result.image.Height(), fixture.height, "height from SOF");
  Expect(result.image.IsOpaque(), "a JPEG has no alpha channel");

  const Difference difference = CompareToReference(result.image, fixture);
  Expect(difference.worst <= worst_allowed,
         std::string(what) + ": worst channel difference from libjpeg is " +
             std::to_string(difference.worst));
  Expect(difference.mean <= mean_allowed,
         std::string(what) + ": mean channel difference from libjpeg is " +
             std::to_string(difference.mean));
}

// Flips one byte and requires the decoder to survive it, which is what the
// sanitizers are watching while this runs. A JPEG is far less self-checking
// than a PNG — there is no CRC — so most corruptions produce a picture rather
// than a failure, and the requirement is only that the picture is inside the
// buffer that was allocated for it.
void ExpectSurvivesCorruption(const std::vector<std::byte>& original, std::size_t stride) {
  for (std::size_t index = 0; index < original.size(); index += stride) {
    std::vector<std::byte> corrupted = original;
    corrupted[index] = static_cast<std::byte>(static_cast<std::uint8_t>(corrupted[index]) ^ 0xFFu);
    const JpegDecodeResult result = DecodeJpeg(corrupted);
    if (result.Ok()) {
      Expect(result.image.Width() > 0 && result.image.Height() > 0,
             "a successful decode must produce a real image");
      volatile std::uint32_t sink = 0;
      for (int y = 0; y < result.image.Height(); ++y) {
        const std::uint32_t* row = result.image.Row(y);
        for (int x = 0; x < result.image.Width(); ++x) {
          sink = sink ^ row[x];
        }
      }
      (void)sink;
    }
  }
}

}  // namespace

void RegisterJpegDecoderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Jpeg/RecognizesItsOwnSignature", [] {
    Expect(gfx::LooksLikeJpeg(GrayFixture().bytes), "a JPEG looks like one");
    const std::vector<std::byte> not_jpeg(16, std::byte{0x42});
    Expect(!gfx::LooksLikeJpeg(not_jpeg), "and other bytes do not");
    Expect(!gfx::LooksLikeJpeg({}), "nor does nothing");
    // SOI and then something that is not a marker. Two bytes of signature match
    // far too much to be worth trusting.
    const std::vector<std::byte> soi_only = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0x00}};
    Expect(!gfx::LooksLikeJpeg(soi_only), "SOI alone is not enough");
  });

  // The bounds below are the measured differences with one level of headroom.
  // Measured: 1 and 0.02 greyscale, 2 and 0.04 at 4:4:4, 3 and 0.44 on the
  // three subsampled fixtures. They are this tight on purpose — a chroma plane
  // off by one row costs a mean in the tens, and a bound of 8 would have let it
  // through while still reading like a real check.
  AddTest(tests, "Jpeg/DecodesGreyscale",
          [] { ExpectMatchesReference(GrayFixture(), "greyscale", 2, 0.1); });

  AddTest(tests, "Jpeg/DecodesFullChroma",
          [] { ExpectMatchesReference(Yuv444Fixture(), "4:4:4", 3, 0.2); });

  AddTest(tests, "Jpeg/DecodesSubsampledChroma",
          [] { ExpectMatchesReference(Yuv420Fixture(), "4:2:0", 4, 0.8); });

  AddTest(tests, "Jpeg/DecodesProgressive",
          [] { ExpectMatchesReference(ProgressiveFixture(), "progressive", 4, 0.8); });

  AddTest(tests, "Jpeg/DecodesAcrossRestartMarkers",
          [] { ExpectMatchesReference(RestartsFixture(), "restarts", 4, 0.8); });

  AddTest(tests, "Jpeg/ProgressiveAgreesWithBaseline", [] {
    // The same picture, coded both ways by the same encoder. This is the test
    // that catches a successive-approximation bug: a progressive decoder can be
    // wrong in a way that still looks like a photograph, and only the baseline
    // of the same image says so.
    const JpegFixture progressive = ProgressiveFixture();
    const JpegDecodeResult result = DecodeJpeg(progressive.bytes);
    Expect(result.Ok(), "progressive decode failed");
    const Difference difference = CompareToReference(result.image, progressive);
    Expect(difference.mean <= 0.8, "progressive mean difference " + std::to_string(difference.mean));
  });

  AddTest(tests, "Jpeg/RejectsWhatItDoesNotImplement", [] {
    // Arithmetic coding. SOF9 where SOF0 was, and nothing else changed.
    std::vector<std::byte> arithmetic = GrayFixture().bytes;
    for (std::size_t i = 0; i + 1 < arithmetic.size(); ++i) {
      if (static_cast<std::uint8_t>(arithmetic[i]) == 0xFF &&
          static_cast<std::uint8_t>(arithmetic[i + 1]) == 0xC0) {
        arithmetic[i + 1] = std::byte{0xC9};
        break;
      }
    }
    const JpegDecodeResult result = DecodeJpeg(arithmetic);
    Expect(!result.Ok(), "arithmetic coding is refused rather than guessed at");
  });

  AddTest(tests, "Jpeg/RefusesAnImageTooLargeToDecode", [] {
    // 32768 x 32768 is a gigabyte of pixels from four bytes of header. The
    // refusal has to happen before the allocation, which is what ASan is
    // watching here: a decoder that allocated first would be killed.
    std::vector<std::byte> huge = GrayFixture().bytes;
    for (std::size_t i = 0; i + 9 < huge.size(); ++i) {
      if (static_cast<std::uint8_t>(huge[i]) == 0xFF &&
          static_cast<std::uint8_t>(huge[i + 1]) == 0xC0) {
        huge[i + 5] = std::byte{0x80};  // height high byte
        huge[i + 6] = std::byte{0x00};
        huge[i + 7] = std::byte{0x80};  // width high byte
        huge[i + 8] = std::byte{0x00};
        break;
      }
    }
    const JpegDecodeResult result = DecodeJpeg(huge);
    Expect(!result.Ok(), "an image past the pixel bound is refused");
  });

  AddTest(tests, "Jpeg/SurvivesTruncationAtEveryLength", [] {
    const JpegFixture fixture = Yuv420Fixture();
    for (std::size_t length = 0; length <= fixture.bytes.size(); ++length) {
      const std::vector<std::byte> truncated(fixture.bytes.begin(),
                                             fixture.bytes.begin() +
                                                 static_cast<std::ptrdiff_t>(length));
      const JpegDecodeResult result = DecodeJpeg(truncated);
      if (result.Ok()) {
        // A JPEG truncated inside its scan is the commonest malformed image on
        // the web, and it is supposed to decode to a partial picture.
        ExpectEqInt(result.image.Width(), fixture.width, "a partial decode keeps its size");
      }
    }
  });

  AddTest(tests, "Jpeg/SurvivesCorruption", [] {
    ExpectSurvivesCorruption(GrayFixture().bytes, 1);
    ExpectSurvivesCorruption(Yuv420Fixture().bytes, 1);
    ExpectSurvivesCorruption(ProgressiveFixture().bytes, 1);
  });

  AddTest(tests, "Jpeg/RejectsAnEmptyQuantisationTable", [] {
    // A file whose SOS names a quantisation table no DQT defined. Dividing by a
    // table that is not there is how a decoder ends up dividing by zero.
    std::vector<std::byte> without_dqt;
    const JpegFixture fixture = GrayFixture();
    for (std::size_t i = 0; i < fixture.bytes.size();) {
      if (static_cast<std::uint8_t>(fixture.bytes[i]) == 0xFF && i + 3 < fixture.bytes.size() &&
          static_cast<std::uint8_t>(fixture.bytes[i + 1]) == 0xDB) {
        const std::size_t length =
            (static_cast<std::size_t>(static_cast<std::uint8_t>(fixture.bytes[i + 2])) << 8) |
            static_cast<std::uint8_t>(fixture.bytes[i + 3]);
        i += 2 + length;
        continue;
      }
      without_dqt.push_back(fixture.bytes[i]);
      ++i;
    }
    const JpegDecodeResult result = DecodeJpeg(without_dqt);
    Expect(!result.Ok(), "a missing quantisation table is a failed decode");
  });
}

}  // namespace microbrowser::tests
