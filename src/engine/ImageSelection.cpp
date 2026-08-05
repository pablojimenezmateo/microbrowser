#include "engine/ImageSelection.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microbrowser::engine {
namespace {

// A `srcset` is attacker-controlled text of unbounded length, and every
// candidate in it is a URL this browser may go and fetch. The parse is linear
// either way; this bound is on what the *page* gets to make the browser do,
// which is the number of requests rather than the time spent parsing.
constexpr std::size_t kMaxCandidates = 64;

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// One descriptor: `2x`, `640w`, or nothing at all.
//
// `h` is parsed and then rejected: it is in the grammar, it is meaningless
// without a `w` beside it, and no browser uses it for selection. Accepting it
// silently as a density would pick a 640-times-too-large image.
bool ApplyDescriptor(std::string_view descriptor, ImageCandidate& candidate) {
  if (descriptor.empty()) {
    return true;  // no descriptor is 1x, which is the candidate's default
  }
  const char unit = descriptor.back();
  const std::optional<float> value = util::ParseFloat(descriptor.substr(0, descriptor.size() - 1));
  if (!value.has_value() || !(*value > 0.0f)) {
    return false;
  }
  if (unit == 'w') {
    candidate.width = *value;
    candidate.has_width = true;
    return true;
  }
  if (unit == 'x') {
    candidate.density = *value;
    return true;
  }
  return false;
}

// The index just past the component of `text` starting at `from`, where a
// component ends at a top-level comma. Parenthesis-aware, because a `sizes`
// entry may hold a media condition and a media condition holds parentheses.
std::size_t ComponentEnd(std::string_view text, std::size_t from) {
  int depth = 0;
  for (std::size_t at = from; at < text.size(); ++at) {
    if (text[at] == '(') {
      ++depth;
    } else if (text[at] == ')') {
      depth = std::max(0, depth - 1);
    } else if (text[at] == ',' && depth == 0) {
      return at;
    }
  }
  return text.size();
}

}  // namespace

std::vector<ImageCandidate> ParseSrcset(std::string_view srcset) {
  std::vector<ImageCandidate> candidates;
  std::size_t at = 0;
  while (at < srcset.size() && candidates.size() < kMaxCandidates) {
    // Whitespace and commas both separate, and a run of either is one
    // separator. This is what makes `a.png,,  b.png` two candidates.
    while (at < srcset.size() && (IsAsciiSpace(srcset[at]) || srcset[at] == ',')) {
      ++at;
    }
    if (at >= srcset.size()) {
      break;
    }
    const std::size_t url_start = at;
    while (at < srcset.size() && !IsAsciiSpace(srcset[at])) {
      ++at;
    }
    std::string_view url = srcset.substr(url_start, at - url_start);

    // A URL that ends in a comma has no descriptor: the comma is the separator
    // that follows it. A URL that does not may be followed by one, up to the
    // next top-level comma. Getting this backwards is what makes a URL with a
    // comma in it -- which reddit's preview service produces -- parse as two
    // candidates, neither of which is a URL.
    std::string_view descriptor;
    if (!url.empty() && url.back() == ',') {
      while (!url.empty() && url.back() == ',') {
        url.remove_suffix(1);
      }
    } else {
      const std::size_t end = ComponentEnd(srcset, at);
      descriptor = util::TrimAscii(srcset.substr(at, end - at));
      at = end;
    }
    if (url.empty()) {
      continue;
    }
    ImageCandidate candidate;
    candidate.url = std::string(url);
    // A descriptor this grammar does not accept drops the candidate rather
    // than the attribute: a page with one typo in a five-entry srcset still
    // gets an image.
    if (ApplyDescriptor(descriptor, candidate)) {
      candidates.push_back(std::move(candidate));
    }
  }
  return candidates;
}

float ParseSizes(std::string_view sizes, const css::MediaContext& context) {
  std::size_t at = 0;
  while (at < sizes.size()) {
    const std::size_t end = ComponentEnd(sizes, at);
    const std::string_view component = util::TrimAscii(sizes.substr(at, end - at));
    at = end + 1;
    if (component.empty()) {
      continue;
    }
    // The source size is the last thing in the component and the media
    // condition is everything before it. Splitting from the right rather than
    // the left is what makes `(min-width: 40em) 50vw` one condition and one
    // length instead of three words.
    std::size_t split = component.size();
    while (split > 0 && !IsAsciiSpace(component[split - 1])) {
      --split;
    }
    const std::string_view length = component.substr(split);
    const std::string_view condition = util::TrimAscii(component.substr(0, split));
    if (!css::MediaQueryListMatches(condition, context)) {
      continue;
    }
    // `calc()` in a `sizes` value resolves to nothing here, so the entry is
    // dropped and the next one -- or the 100vw default -- answers. A wrong
    // number would be worse: it scales every candidate's density.
    const std::optional<float> resolved = css::ResolveAbsoluteLength(length, context);
    if (resolved.has_value() && *resolved > 0.0f) {
      return *resolved;
    }
  }
  // The spec's default source size is 100vw, and it is the reason a `w`
  // descriptor srcset with no `sizes` attribute still selects sensibly.
  return context.viewport_width;
}

bool ImageTypeIsSupported(std::string_view mime_type) {
  // The formats src/gfx decodes, and no others. This list and the sniffing in
  // Engine::DecodePendingImages are the same fact stated twice; a decoder that
  // lands without changing both makes `<picture>` decline a format the browser
  // can in fact display. ImageSelectionTests asserts the list, so adding one is
  // a failing test rather than a silent divergence -- ADR 0023 §5 has the order
  // they arrive in, and `image/gif` is next.
  const std::string_view type = util::TrimAscii(mime_type);
  return util::EqualsAsciiCaseInsensitive(type, "image/png") ||
         util::EqualsAsciiCaseInsensitive(type, "image/jpeg") ||
         util::EqualsAsciiCaseInsensitive(type, "image/svg+xml");
}

std::string SelectImageSource(const dom::Element& image, const css::MediaContext& context) {
  std::vector<ImageCandidate> candidates;
  std::string_view sizes;
  bool from_source = false;

  // `<picture>` first: a matching `<source>` replaces the img's own srcset
  // entirely, and only the sources *before* the img are considered -- which is
  // why this walks the parent's children rather than asking for its sources.
  const dom::Node* parent = image.Parent();
  const auto* picture = parent != nullptr && parent->IsElement()
                            ? static_cast<const dom::Element*>(parent)
                            : nullptr;
  if (picture != nullptr && util::EqualsAsciiCaseInsensitive(picture->TagName(), "picture")) {
    for (const std::unique_ptr<dom::Node>& child : picture->Children()) {
      if (child.get() == &image) {
        break;
      }
      if (!child->IsElement()) {
        continue;
      }
      const auto& element = static_cast<const dom::Element&>(*child);
      if (!util::EqualsAsciiCaseInsensitive(element.TagName(), "source")) {
        continue;
      }
      const std::string* type = element.GetAttribute("type");
      if (type != nullptr && !ImageTypeIsSupported(*type)) {
        continue;
      }
      const std::string* media = element.GetAttribute("media");
      if (media != nullptr && !css::MediaQueryListMatches(*media, context)) {
        continue;
      }
      const std::string* srcset = element.GetAttribute("srcset");
      if (srcset == nullptr) {
        continue;
      }
      candidates = ParseSrcset(*srcset);
      if (candidates.empty()) {
        continue;
      }
      const std::string* source_sizes = element.GetAttribute("sizes");
      sizes = source_sizes == nullptr ? std::string_view{} : std::string_view{*source_sizes};
      from_source = true;
      break;
    }
  }

  if (!from_source) {
    if (const std::string* srcset = image.GetAttribute("srcset"); srcset != nullptr) {
      candidates = ParseSrcset(*srcset);
    }
    if (const std::string* image_sizes = image.GetAttribute("sizes"); image_sizes != nullptr) {
      sizes = *image_sizes;
    }
    // `src` is a 1x candidate, but only when the srcset does not already name
    // one and names no width descriptors. An author who wrote both means `src`
    // as the fallback for a browser with no srcset, and this browser has one.
    const std::string* src = image.GetAttribute("src");
    const bool has_width = std::any_of(candidates.begin(), candidates.end(),
                                       [](const ImageCandidate& c) { return c.has_width; });
    const bool has_1x = std::any_of(candidates.begin(), candidates.end(),
                                    [](const ImageCandidate& c) {
                                      return !c.has_width && c.density == 1.0f;
                                    });
    if (src != nullptr && !src->empty() && !has_width && !has_1x &&
        candidates.size() < kMaxCandidates) {
      candidates.push_back(ImageCandidate{*src, 1.0f, 0.0f, false});
    }
  }

  if (candidates.empty()) {
    return {};
  }

  // Normalise: a `w` descriptor is a density once the source size is known.
  // The source size is only consulted when something needs it, because
  // evaluating `sizes` means evaluating a media query per entry.
  const bool any_width = std::any_of(candidates.begin(), candidates.end(),
                                     [](const ImageCandidate& c) { return c.has_width; });
  if (any_width) {
    const float source_size = ParseSizes(sizes, context);
    for (ImageCandidate& candidate : candidates) {
      if (candidate.has_width) {
        candidate.density =
            source_size > 0.0f ? candidate.width / source_size : candidate.density;
      }
    }
  }

  // The lowest density that is at least the device's, or the highest there is.
  // Ties go to the first written, which is document order -- and the reason
  // this is a scan rather than a sort, which would not be stable about that.
  const float target = context.device_pixel_ratio > 0.0f ? context.device_pixel_ratio : 1.0f;
  const ImageCandidate* best = nullptr;
  for (const ImageCandidate& candidate : candidates) {
    if (best == nullptr) {
      best = &candidate;
      continue;
    }
    const bool best_reaches = best->density >= target;
    const bool reaches = candidate.density >= target;
    if (reaches && best_reaches) {
      best = candidate.density < best->density ? &candidate : best;
    } else if (reaches) {
      best = &candidate;
    } else if (!best_reaches) {
      best = candidate.density > best->density ? &candidate : best;
    }
  }
  return best->url;
}

bool ImageLoadingIsLazy(const dom::Element& image) {
  const std::string* attribute = image.GetAttribute("loading");
  return attribute != nullptr && util::EqualsAsciiCaseInsensitive(*attribute, "lazy");
}

}  // namespace microbrowser::engine
