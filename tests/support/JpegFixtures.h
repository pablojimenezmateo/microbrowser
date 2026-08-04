#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace microbrowser::tests {

// JPEG files for the decoder tests, and the pixels an independent decoder gets
// out of them.
//
// Unlike tests/support/SyntheticPng.h these are not built here. The reason is
// in tools/make-jpeg-fixtures.py, which generates JpegFixtures.cpp: a builder
// written next to the decoder proves the two are inverses, and what needs
// proving is that this decoder agrees with the encoders that produced the web.
// Deliberately-wrong inputs are made by corrupting these, which is how the PNG
// tests cover the same ground.

struct JpegFixture {
  int width = 0;
  int height = 0;
  std::vector<std::byte> bytes;
  // Tightly packed RGB, three bytes per pixel, width * height * 3 long.
  std::vector<std::uint8_t> rgb;
};

// Greyscale, one component: no upsampling, no colour conversion.
JpegFixture GrayFixture();
// Three components with no chroma subsampling.
JpegFixture Yuv444Fixture();
// 4:2:0 at 17x9, so the last MCU is padding and chroma is upsampled 2x.
JpegFixture Yuv420Fixture();
// Progressive: spectral selection and successive approximation.
JpegFixture ProgressiveFixture();
// Baseline with a restart interval.
JpegFixture RestartsFixture();

}  // namespace microbrowser::tests
