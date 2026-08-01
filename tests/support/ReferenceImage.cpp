#include "support/ReferenceImage.h"

#include <cstdint>
#include <fstream>
#include <sstream>

#include "TestSupport.h"

namespace microbrowser::tests {

namespace {

// "P6\n<w> <h>\n255\n" then w*h RGB triples.
struct PpmHeader {
  bool valid = false;
  int width = 0;
  int height = 0;
  std::size_t pixel_offset = 0;
};

PpmHeader ParsePpmHeader(const std::string& data) {
  PpmHeader header;
  std::istringstream stream(data);
  std::string magic;
  int max_value = 0;
  if (!(stream >> magic >> header.width >> header.height >> max_value)) {
    return header;
  }
  if (magic != "P6" || max_value != 255 || header.width <= 0 || header.height <= 0) {
    return header;
  }
  // Exactly one whitespace byte separates the header from the payload.
  header.pixel_offset = static_cast<std::size_t>(stream.tellg()) + 1;
  header.valid = header.pixel_offset <= data.size();
  return header;
}

}  // namespace

std::string EncodePpm(const gfx::Canvas& canvas) {
  std::string out;
  const int width = canvas.Width();
  const int height = canvas.Height();
  out.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 + 32);

  out += "P6\n";
  out += std::to_string(width);
  out += ' ';
  out += std::to_string(height);
  out += "\n255\n";

  for (int y = 0; y < height; ++y) {
    const std::uint32_t* row = canvas.Row(y);
    for (int x = 0; x < width; ++x) {
      const std::uint32_t argb = row[x];
      // Alpha is dropped: a reference image is what the user sees, and what the
      // user sees has already been composited onto the window.
      out += static_cast<char>((argb >> 16) & 0xFFu);
      out += static_cast<char>((argb >> 8) & 0xFFu);
      out += static_cast<char>(argb & 0xFFu);
    }
  }
  return out;
}

ComparisonResult ComparePpm(const std::string& actual, const std::string& expected) {
  ComparisonResult result;

  const PpmHeader actual_header = ParsePpmHeader(actual);
  const PpmHeader expected_header = ParsePpmHeader(expected);
  if (!actual_header.valid || !expected_header.valid) {
    result.message = "one of the images is not a valid P6 PPM";
    return result;
  }
  if (actual_header.width != expected_header.width ||
      actual_header.height != expected_header.height) {
    result.message = "size mismatch: actual " + std::to_string(actual_header.width) + "x" +
                     std::to_string(actual_header.height) + ", expected " +
                     std::to_string(expected_header.width) + "x" +
                     std::to_string(expected_header.height);
    return result;
  }

  const std::size_t pixels =
      static_cast<std::size_t>(actual_header.width) * static_cast<std::size_t>(actual_header.height);
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t a = actual_header.pixel_offset + i * 3;
    const std::size_t e = expected_header.pixel_offset + i * 3;
    if (a + 3 > actual.size() || e + 3 > expected.size()) {
      result.message = "image data is truncated";
      return result;
    }
    if (actual.compare(a, 3, expected, e, 3) != 0) {
      ++result.differing_pixels;
      if (result.first_x < 0) {
        result.first_x = static_cast<int>(i % static_cast<std::size_t>(actual_header.width));
        result.first_y = static_cast<int>(i / static_cast<std::size_t>(actual_header.width));
      }
    }
  }

  result.matches = result.differing_pixels == 0;
  if (!result.matches) {
    result.message = std::to_string(result.differing_pixels) + " pixel(s) differ, first at (" +
                     std::to_string(result.first_x) + ", " + std::to_string(result.first_y) + ")";
  }
  return result;
}

std::filesystem::path ReferenceDirectory() {
  return SourceRoot() / "tests" / "ref";
}

ComparisonResult CompareAgainstGolden(const gfx::Canvas& canvas, const std::string& name) {
  const std::filesystem::path golden = ReferenceDirectory() / (name + ".ppm");
  const std::filesystem::path actual_path = ReferenceDirectory() / (name + ".actual.ppm");
  const std::string actual = EncodePpm(canvas);

  // Goldens are grouped into subdirectories by feature ("path/circle"), so the
  // failure path has to be able to create one — otherwise the first test of a
  // new group reports "cannot open file" instead of "here is what it drew".
  if (actual_path.has_parent_path()) {
    std::error_code create_error;
    std::filesystem::create_directories(actual_path.parent_path(), create_error);
  }

  if (!std::filesystem::exists(golden)) {
    WriteFile(actual_path, actual);
    ComparisonResult result;
    result.message = "no golden at " + golden.string() +
                     "; wrote the rendered output to " + actual_path.string() +
                     ". Inspect it, and if it is correct, rename it to the golden path.";
    return result;
  }

  ComparisonResult result = ComparePpm(actual, ReadFile(golden));
  if (!result.matches) {
    WriteFile(actual_path, actual);
    result.message += "; wrote actual output to " + actual_path.string();
  } else {
    std::error_code ec;
    std::filesystem::remove(actual_path, ec);
  }
  return result;
}

}  // namespace microbrowser::tests
