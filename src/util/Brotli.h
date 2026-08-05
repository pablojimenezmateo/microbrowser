#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace microbrowser::util {

// Brotli, decode only, under a bound.
//
// **The one third-party decoder in this tree, and the reason is measurement.**
// ADR 0024 sanctions it, and ADR 0001's rule is that a dependency has to earn its
// place: brotli earns it twice over. It is what `Content-Encoding: br` is -- which
// is what the web actually serves -- and it is the inner half of WOFF2, which is
// what every web font actually ships as. Writing a brotli decoder by hand would be
// a second implementation of a format whose reference decoder is the one every
// server was tested against, and its failure mode is a wrong byte in a font rather
// than a refusal.
//
// It is *decode only*: `libbrotlienc` is not linked, and nothing here compresses.
// A browser has no reason to.
//
// The bound is ADR 0010's, unchanged: an absolute ceiling on the output, checked
// as it is produced rather than after, so a stream that would expand past it stops
// mid-way and fails. There is no size field in a brotli stream to refuse from --
// unlike gzip's ISIZE -- so the ceiling is the only defence and it has to be
// enforced *during* the decode. That difference is why this is a separate function
// rather than a case in Inflate.h.
//
// False on a malformed stream, a truncated one, or one that would exceed
// `max_output` -- and `out` is left **empty**, not partial. A partially decoded
// stream is not a shorter document, it is a document that means something else, and
// emptying it is what makes "fails rather than truncates" true at the call site
// rather than in a comment. The fuzz target asserts exactly that.
bool BrotliInflate(std::span<const std::byte> input, std::size_t max_output,
                   std::vector<std::byte>& out);

}  // namespace microbrowser::util
