#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "util/Brotli.h"

// A brotli stream, from a server.
//
// The decoder is a third-party one and this target is not doubting it -- it is
// checking the *bound*, which is ours. ADR 0010's rule is that a decompressor
// fails rather than truncates, and brotli is the one coding with no declared
// output size to refuse from: gzip's ISIZE lets a bomb be rejected before a byte
// is produced, and a brotli stream says nothing about how large it becomes. So the
// ceiling has to be enforced *during* the decode, and the property this asserts is
// the one that makes that correct:
//
//   **the output never exceeds the ceiling, whatever the input.**
//
// Nothing else here is checkable without a compressor: a stream that decodes is
// whatever it decodes to. What is checkable, and what a bug here would break, is
// that a refusal is a refusal -- `out` is not a shorter document, and a caller that
// read it after a false return would be reading a document that means something
// else.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 2) {
    return 0;
  }
  using namespace microbrowser;

  // The ceiling comes out of the input, so the fuzzer explores the boundary rather
  // than one arbitrary limit. Bounded well below anything a real response uses, so
  // a stream that expands is refused rather than allocated.
  const std::size_t ceiling = static_cast<std::size_t>(data[0]) * 256u + 1u;
  const std::span<const std::byte> stream(reinterpret_cast<const std::byte*>(data + 1), size - 1);

  std::vector<std::byte> out;
  const bool ok = util::BrotliInflate(stream, ceiling, out);
  if (ok && out.size() > ceiling) {
    __builtin_trap();  // the ceiling is the whole contract
  }
  if (!ok && !out.empty()) {
    // A refusal must not hand back a partial document. This is the property that
    // makes "fails rather than truncates" true at the call site rather than in a
    // comment.
    __builtin_trap();
  }

  // The same stream against a ceiling of zero: nothing can be produced, so nothing
  // may be.
  std::vector<std::byte> none;
  if (util::BrotliInflate(stream, 0, none) && !none.empty()) {
    __builtin_trap();
  }
  return 0;
}
