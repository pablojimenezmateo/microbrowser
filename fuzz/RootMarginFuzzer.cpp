#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "bindings/Geometry.h"
#include "bindings/ViewObservers.h"

// `IntersectionObserver`'s `rootMargin`, fed arbitrary bytes.
//
// It is the only text this browser parses on the way to deciding *which images
// to fetch*, which is why it earns a target of its own. The property is not
// "does not crash" -- a whitespace split and four `strtod` calls were never
// going to overrun anything. It is that the four numbers coming out are
// **finite and bounded**, because the failure mode here is arithmetic rather
// than memory:
//
//   `rootMargin: '1e300px'` parses as a finite double and narrows to `inf`.
//   An infinite root bound makes every intersection ratio `inf/inf`, and a NaN
//   compares false against every threshold -- so the observer goes *silent*
//   rather than firing wrongly, and a feed simply stops loading. ADR 0018 §5's
//   whole rule is that an observer which never fires is worse than one that
//   does not exist, and that is exactly how a page would produce one.
namespace {

using microbrowser::bindings::GeometryRect;
using microbrowser::bindings::RootMargin;

bool Sane(float value) { return std::isfinite(value) && value >= -1.0e6f && value <= 1.0e6f; }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // Two roots: one ordinary, and one whose size is itself extreme, since a
  // percentage margin multiplies by it.
  const GeometryRect roots[] = {GeometryRect{0.0f, 0.0f, 1280.0f, 900.0f},
                                GeometryRect{0.0f, 0.0f, 1.0e30f, 1.0e30f}};
  for (const GeometryRect& root : roots) {
    const RootMargin margin = microbrowser::bindings::ParseRootMargin(input, root);
    if (!Sane(margin.top) || !Sane(margin.right) || !Sane(margin.bottom) || !Sane(margin.left)) {
      __builtin_trap();  // a margin no layout could mean, or not a number at all
    }
    const GeometryRect expanded = microbrowser::bindings::ExpandedBy(root, margin);
    if (!std::isfinite(expanded.x) || !std::isfinite(expanded.y) ||
        !std::isfinite(expanded.width) || !std::isfinite(expanded.height)) {
      // The rectangle every ratio is divided by. Not-a-number here is an
      // observer that never fires again.
      __builtin_trap();
    }
  }
  return 0;
}
