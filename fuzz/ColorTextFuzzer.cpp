#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "gfx/Color.h"
#include "gfx/ColorText.h"

// The colour parser, fed arbitrary bytes.
//
// A colour is attacker-controlled text on two separate paths -- a stylesheet's declarations and an
// SVG's `fill` -- and it is a hand-written scanner over indices, which is the shape that reads past
// the end when a suffix test and a `remove_suffix` disagree about a length. `hsl()`, the two
// grammars and the four hex lengths landed on 2026-08-14; this landed with them, which is
// `guidelines/security.md`'s rule rather than a nicety.
//
// Two properties, and the second is the one worth having:
//
//   1. Parsing terminates and stays in bounds for any input. AddressSanitizer decides this one.
//   2. **A parsed colour round-trips.** Serializing one and parsing the result gives the same
//      colour. That is what makes `el.style.color = x; el.style.color` idempotent, which CSSOM
//      requires and which `css/CSS2/syntax/colors-007.html` asserts for a thousand values -- and a
//      serializer that printed more precision than a `Color` holds, or an `rgba()` whose alpha
//      re-parsed one off, would fail it here rather than in a test somebody has to write.

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const std::optional<microbrowser::gfx::Color> color = microbrowser::gfx::ParseColorText(text);
  if (!color.has_value()) {
    return 0;
  }
  const std::string serialized = microbrowser::gfx::SerializeColorText(*color);
  const std::optional<microbrowser::gfx::Color> again =
      microbrowser::gfx::ParseColorText(serialized);
  if (!again.has_value() || again->argb != color->argb) {
    __builtin_trap();
  }
  return 0;
}
