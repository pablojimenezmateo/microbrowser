#include "wpt/Reftest.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace microbrowser::wpt {
namespace {

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\n' ||
                            value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' ||
                            value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

bool ParseNumber(std::string_view value, std::uint32_t* out) {
  value = Trim(value);
  if (value.empty()) {
    return false;
  }
  std::uint64_t result = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
    // A tolerance larger than the whole canvas is not a parse error, but it is
    // not worth carrying either: saturate rather than wrap.
    if (result > 0xFFFFFFFFull) {
      result = 0xFFFFFFFFull;
    }
  }
  *out = static_cast<std::uint32_t>(result);
  return true;
}

// `n` or `low-high`.
bool ParseRange(std::string_view value, FuzzyRange* out) {
  value = Trim(value);
  const std::size_t dash = value.find('-');
  if (dash == std::string_view::npos) {
    std::uint32_t single = 0;
    if (!ParseNumber(value, &single)) {
      return false;
    }
    out->low = single;
    out->high = single;
    return true;
  }
  return ParseNumber(value.substr(0, dash), &out->low) &&
         ParseNumber(value.substr(dash + 1), &out->high);
}

// Strips a `maxDifference=` / `totalPixels=` key, and reports which one it was.
// Upstream accepts the two halves in either order when they are named, and in
// this order when they are not.
std::string_view StripKey(std::string_view part, std::string_view* key) {
  const std::size_t equals = part.find('=');
  if (equals == std::string_view::npos) {
    *key = {};
    return part;
  }
  *key = Trim(part.substr(0, equals));
  return part.substr(equals + 1);
}

// The content of one `<meta>` tag's attribute, unquoted.
std::string_view AttributeValue(std::string_view tag, std::string_view name) {
  std::size_t position = 0;
  while (true) {
    const std::size_t at = tag.find(name, position);
    if (at == std::string_view::npos) {
      return {};
    }
    std::size_t cursor = at + name.size();
    while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) {
      ++cursor;
    }
    if (cursor >= tag.size() || tag[cursor] != '=') {
      position = at + name.size();
      continue;
    }
    ++cursor;
    while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) {
      ++cursor;
    }
    char quote = '\0';
    if (cursor < tag.size() && (tag[cursor] == '"' || tag[cursor] == '\'')) {
      quote = tag[cursor];
      ++cursor;
    }
    std::size_t end = cursor;
    while (end < tag.size()) {
      if (quote != '\0' ? tag[end] == quote : (tag[end] == ' ' || tag[end] == '\t' || tag[end] == '>')) {
        break;
      }
      ++end;
    }
    return tag.substr(cursor, end - cursor);
  }
}

std::uint32_t ChannelDifference(std::uint32_t left, std::uint32_t right) {
  return left > right ? left - right : right - left;
}

}  // namespace

bool ParseFuzzyRanges(std::string_view content, FuzzyAllowance* out) {
  content = Trim(content);
  const std::size_t semicolon = content.find(';');
  if (semicolon == std::string_view::npos) {
    return false;
  }
  std::string_view first_key;
  std::string_view second_key;
  const std::string_view first = StripKey(content.substr(0, semicolon), &first_key);
  const std::string_view second = StripKey(content.substr(semicolon + 1), &second_key);
  FuzzyRange a;
  FuzzyRange b;
  if (!ParseRange(first, &a) || !ParseRange(second, &b)) {
    return false;
  }
  // Named halves may be given in either order; unnamed ones are positional.
  if (first_key == "totalPixels" || second_key == "maxDifference") {
    out->total_pixels = a;
    out->max_difference = b;
  } else {
    out->max_difference = a;
    out->total_pixels = b;
  }
  return true;
}

std::vector<FuzzyAnnotation> ParseFuzzy(std::string_view head) {
  std::vector<FuzzyAnnotation> annotations;
  std::size_t position = 0;
  while (position < head.size()) {
    const std::size_t at = head.find("fuzzy", position);
    if (at == std::string_view::npos) {
      break;
    }
    const std::size_t tag_start = head.rfind('<', at);
    const std::size_t tag_end = head.find('>', at);
    if (tag_start == std::string_view::npos || tag_end == std::string_view::npos) {
      break;
    }
    position = tag_end + 1;
    const std::string_view tag = head.substr(tag_start, tag_end - tag_start);
    if (AttributeValue(tag, "name") != "fuzzy") {
      continue;
    }
    std::string_view content = Trim(AttributeValue(tag, "content"));
    if (content.empty()) {
      continue;
    }
    FuzzyAnnotation annotation;
    // `[test==]ref:` scopes the tolerance to one reference. Neither half of a
    // range can contain a colon, so the first one is unambiguously the
    // separator and an unprefixed `0-1;0-2` has none.
    const std::size_t colon = content.find(':');
    if (colon != std::string_view::npos) {
      std::string_view prefix = content.substr(0, colon);
      const std::size_t equals = prefix.find("==");
      if (equals != std::string_view::npos) {
        prefix = prefix.substr(equals + 2);
      }
      annotation.reference = std::string(Trim(prefix));
      content = content.substr(colon + 1);
    }
    if (!ParseFuzzyRanges(content, &annotation.allowance)) {
      // `content="{{ fuzzy }}"`: six files in the checkout are templates whose
      // substitution this server does not do. A skipped annotation is an exact
      // comparison, which fails visibly; a guessed one is a pass nobody chose.
      continue;
    }
    annotations.push_back(std::move(annotation));
  }
  return annotations;
}

std::string SerializeFuzzy(const FuzzyAllowance& allowance) {
  if (allowance.IsExact()) {
    return {};
  }
  return std::to_string(allowance.max_difference.low) + "-" +
         std::to_string(allowance.max_difference.high) + ";" +
         std::to_string(allowance.total_pixels.low) + "-" +
         std::to_string(allowance.total_pixels.high);
}

ImageDifference CompareCanvases(const gfx::Canvas& actual, const gfx::Canvas& expected) {
  ImageDifference difference;
  if (actual.Width() != expected.Width() || actual.Height() != expected.Height()) {
    difference.max_per_channel = 255;
    const std::uint64_t left = static_cast<std::uint64_t>(std::max(actual.Width(), 0)) *
                               static_cast<std::uint64_t>(std::max(actual.Height(), 0));
    const std::uint64_t right = static_cast<std::uint64_t>(std::max(expected.Width(), 0)) *
                                static_cast<std::uint64_t>(std::max(expected.Height(), 0));
    difference.pixels_different = std::max(left, right);
    return difference;
  }
  for (int y = 0; y < actual.Height(); ++y) {
    const std::uint32_t* actual_row = actual.Row(y);
    const std::uint32_t* expected_row = expected.Row(y);
    if (actual_row == nullptr || expected_row == nullptr) {
      continue;
    }
    for (int x = 0; x < actual.Width(); ++x) {
      const std::uint32_t left = actual_row[x];
      const std::uint32_t right = expected_row[x];
      if (left == right) {
        continue;
      }
      const std::uint32_t channels[3] = {
          ChannelDifference((left >> 16) & 0xFFu, (right >> 16) & 0xFFu),
          ChannelDifference((left >> 8) & 0xFFu, (right >> 8) & 0xFFu),
          ChannelDifference(left & 0xFFu, right & 0xFFu),
      };
      const std::uint32_t worst = std::max({channels[0], channels[1], channels[2]});
      if (worst == 0) {
        // The two differ only in alpha, which is not a visible difference.
        continue;
      }
      ++difference.pixels_different;
      difference.max_per_channel = std::max(difference.max_per_channel, worst);
    }
  }
  return difference;
}

bool FuzzyAllows(const ImageDifference& difference, const FuzzyAllowance& allowance) {
  if (allowance.IsExact()) {
    return difference.pixels_different == 0;
  }
  if (difference.pixels_different == 0 && allowance.total_pixels.low == 0) {
    return true;
  }
  if (difference.max_per_channel == 0 && allowance.max_difference.low == 0) {
    return true;
  }
  return allowance.max_difference.low <= difference.max_per_channel &&
         difference.max_per_channel <= allowance.max_difference.high &&
         allowance.total_pixels.low <= difference.pixels_different &&
         difference.pixels_different <= allowance.total_pixels.high;
}

gfx::Canvas DifferenceImage(const gfx::Canvas& actual, const gfx::Canvas& expected) {
  const int width = std::max(actual.Width(), expected.Width());
  const int height = std::max(actual.Height(), expected.Height());
  gfx::Canvas diff{width, height};
  if (diff.IsEmpty()) {
    return diff;
  }
  for (int y = 0; y < height; ++y) {
    std::uint32_t* out = diff.Row(y);
    const std::uint32_t* actual_row = y < actual.Height() ? actual.Row(y) : nullptr;
    const std::uint32_t* expected_row = y < expected.Height() ? expected.Row(y) : nullptr;
    for (int x = 0; x < width; ++x) {
      const bool has_actual = actual_row != nullptr && x < actual.Width();
      const bool has_expected = expected_row != nullptr && x < expected.Width();
      const std::uint32_t left = has_actual ? actual_row[x] : 0xFF000000u;
      const std::uint32_t right = has_expected ? expected_row[x] : 0xFF000000u;
      std::uint32_t worst = 255;
      if (has_actual && has_expected) {
        worst = std::max({ChannelDifference((left >> 16) & 0xFFu, (right >> 16) & 0xFFu),
                          ChannelDifference((left >> 8) & 0xFFu, (right >> 8) & 0xFFu),
                          ChannelDifference(left & 0xFFu, right & 0xFFu)});
      }
      if (worst == 0) {
        // The reference, washed out to a quarter of its contrast, so the
        // difference is read against the page it happened on rather than
        // against nothing.
        const std::uint32_t red = (right >> 16) & 0xFFu;
        const std::uint32_t green = (right >> 8) & 0xFFu;
        const std::uint32_t blue = right & 0xFFu;
        const std::uint32_t luma = (red * 77 + green * 150 + blue * 29) >> 8;
        const std::uint32_t washed = 255 - (255 - luma) / 4;
        out[x] = 0xFF000000u | (washed << 16) | (washed << 8) | washed;
        continue;
      }
      // Yellow at one level, red at 255: the magnitude has to be visible, or a
      // tolerance and a missing feature are the same picture.
      out[x] = 0xFFFF0000u | ((255u - worst) << 8);
    }
  }
  return diff;
}

bool WritePpm(const gfx::Canvas& canvas, const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  std::fprintf(file, "P6\n%d %d\n255\n", canvas.Width(), canvas.Height());
  std::string row;
  row.reserve(static_cast<std::size_t>(std::max(canvas.Width(), 0)) * 3);
  for (int y = 0; y < canvas.Height(); ++y) {
    const std::uint32_t* pixels = canvas.Row(y);
    row.clear();
    for (int x = 0; x < canvas.Width(); ++x) {
      const std::uint32_t argb = pixels == nullptr ? 0xFFFFFFFFu : pixels[x];
      row.push_back(static_cast<char>((argb >> 16) & 0xFFu));
      row.push_back(static_cast<char>((argb >> 8) & 0xFFu));
      row.push_back(static_cast<char>(argb & 0xFFu));
    }
    if (std::fwrite(row.data(), 1, row.size(), file) != row.size()) {
      std::fclose(file);
      return false;
    }
  }
  return std::fclose(file) == 0;
}

std::string ArtifactStem(std::string_view url_path) {
  std::string stem;
  stem.reserve(url_path.size());
  for (const char c : url_path) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
    stem.push_back(safe ? c : '_');
  }
  return stem;
}

}  // namespace microbrowser::wpt
