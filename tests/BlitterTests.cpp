#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "gfx/Blitter.h"
#include "gfx/Color.h"

namespace microbrowser::tests {

using gfx::BlendSpanIsVectorized;
using gfx::BlendSpanSrcOver;
using gfx::BlendSpanSrcOverScalar;
using gfx::Color;

namespace {

std::string Hex(std::uint32_t value) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out = "0x";
  for (int shift = 28; shift >= 0; shift -= 4) {
    out += kDigits[(value >> shift) & 0xFu];
  }
  return out;
}

// Runs both implementations over the same destination and requires every byte
// to agree.
void ExpectAgreement(const std::vector<std::uint32_t>& destination, Color source,
                     std::string_view context) {
  std::vector<std::uint32_t> vector_result = destination;
  std::vector<std::uint32_t> scalar_result = destination;
  BlendSpanSrcOver(vector_result.data(), vector_result.size(), source);
  BlendSpanSrcOverScalar(scalar_result.data(), scalar_result.size(), source);

  for (std::size_t i = 0; i < destination.size(); ++i) {
    if (vector_result[i] != scalar_result[i]) {
      Expect(false, std::string(context) + ": pixel " + std::to_string(i) + " of " +
                        std::to_string(destination.size()) + " differs — vector " +
                        Hex(vector_result[i]) + ", scalar " + Hex(scalar_result[i]) +
                        ", source " + Hex(source.argb) + ", destination " +
                        Hex(destination[i]));
    }
  }
}

}  // namespace

void RegisterBlitterTests(std::vector<TestCase>& tests) {
  // Without this, every comparison below is the scalar implementation against
  // itself: green, fast, and proving nothing. SSE2 is baseline on x86-64, so on
  // that target a missing vector path is a build problem, not a portability
  // one. Other targets make no claim until they grow a path of their own.
  AddTest(tests, "Blitter/TheVectorPathIsCompiledInOnTargetsThatHaveOne", [] {
#if defined(__x86_64__) || defined(_M_X64)
    Expect(BlendSpanIsVectorized(),
           "SSE2 is part of the x86-64 baseline, so the vector blitter must be selected here; "
           "if it is not, the equivalence tests below are comparing scalar against scalar");
#else
    Expect(true, "no vector path is claimed for this target");
#endif
  });

  // The exhaustive one. Every source alpha against a spread of destinations,
  // which is where an off-by-one in the rounding identity would hide: the error
  // is at most one level per channel and only appears at particular alphas.
  AddTest(tests, "Blitter/AgreesWithTheScalarReferenceAtEverySourceAlpha", [] {
    const std::vector<std::uint32_t> destination = {
        0x00000000u, 0xFFFFFFFFu, 0xFF000000u, 0x00FFFFFFu, 0x80402010u,
        0x017F80FEu, 0xFEFF0001u, 0x7F7F7F7Fu, 0x01010101u, 0xC3A5966Du,
        0xFF1F6FEBu, 0x40201008u, 0x8000FF00u, 0x0F0F0F0Fu, 0xF0F0F0F0u,
        0x33445566u,
    };
    for (int alpha = 0; alpha <= 255; ++alpha) {
      for (const std::uint32_t rgb : {0x000000u, 0xFFFFFFu, 0x1F6FEBu, 0x7F8081u, 0x010203u}) {
        const Color source{(static_cast<std::uint32_t>(alpha) << 24) | rgb};
        ExpectAgreement(destination, source, "alpha " + std::to_string(alpha));
      }
    }
  });

  // The tail. A vector loop that handles four pixels at a time gets the
  // remainder wrong in exactly one of five ways, and a span whose length is a
  // multiple of four never notices.
  AddTest(tests, "Blitter/AgreesWithTheScalarReferenceAtEveryTailLength", [] {
    const Color source = Color::Rgba(0x12, 0x34, 0x56, 0x9A);
    for (std::size_t length = 0; length <= 17; ++length) {
      std::vector<std::uint32_t> destination(length);
      for (std::size_t i = 0; i < length; ++i) {
        destination[i] = 0xFF000000u | static_cast<std::uint32_t>(i * 0x010203u);
      }
      ExpectAgreement(destination, source, "length " + std::to_string(length));
    }
  });

  AddTest(tests, "Blitter/AgreesWithTheScalarReferenceAtEveryStartOffset", [] {
    // Spans start wherever a shape's left edge falls, so the loads are
    // unaligned far more often than not.
    const Color source = Color::Rgba(0xAB, 0xCD, 0xEF, 0x55);
    std::vector<std::uint32_t> backing(32);
    for (std::size_t i = 0; i < backing.size(); ++i) {
      backing[i] = 0x11223344u * static_cast<std::uint32_t>(i + 1);
    }
    for (std::size_t offset = 0; offset < 8; ++offset) {
      std::vector<std::uint32_t> vector_result = backing;
      std::vector<std::uint32_t> scalar_result = backing;
      const std::size_t length = backing.size() - offset;
      BlendSpanSrcOver(vector_result.data() + offset, length, source);
      BlendSpanSrcOverScalar(scalar_result.data() + offset, length, source);
      Expect(vector_result == scalar_result,
             "an unaligned span must blend identically; offset " + std::to_string(offset));
      for (std::size_t i = 0; i < offset; ++i) {
        ExpectEqInt(vector_result[i], backing[i],
                    "pixels before the span must be untouched, or the vector loop is writing "
                    "backwards past its start");
      }
    }
  });

  AddTest(tests, "Blitter/TheDegenerateAlphasAreExact", [] {
    const std::uint32_t destination_value = 0xFF204060u;

    std::vector<std::uint32_t> opaque = {destination_value};
    BlendSpanSrcOver(opaque.data(), opaque.size(), Color::Rgb(0x11, 0x22, 0x33));
    ExpectEqInt(opaque[0], 0xFF112233u,
                "an opaque source must replace the destination exactly, with no rounding drift");

    std::vector<std::uint32_t> clear = {destination_value};
    BlendSpanSrcOver(clear.data(), clear.size(), Color::Transparent());
    ExpectEqInt(clear[0], destination_value, "a fully transparent source must change nothing");
  });

  AddTest(tests, "Blitter/AZeroLengthSpanTouchesNothing", [] {
    std::vector<std::uint32_t> destination = {0xDEADBEEFu};
    BlendSpanSrcOver(destination.data(), 0, Color::Rgb(0, 0, 0));
    ExpectEqInt(destination[0], 0xDEADBEEFu, "a zero-length span must not write");
  });

  AddTest(tests, "Blitter/BlendingOntoATransparentDestinationBuildsUpAlpha", [] {
    // The case a premultiplied pipeline gets wrong for free and a
    // non-premultiplied one has to get right on purpose.
    std::vector<std::uint32_t> destination = {0x00000000u};
    BlendSpanSrcOver(destination.data(), 1, Color::Rgba(0xFF, 0x00, 0x00, 0x80));
    ExpectEqInt(static_cast<int>((destination[0] >> 24) & 0xFFu), 0x80,
                "compositing onto nothing yields the source alpha");
    ExpectEqInt(static_cast<int>((destination[0] >> 16) & 0xFFu), 0x80,
                "and the source color scaled by it");
  });
}

}  // namespace microbrowser::tests
