#include "util/Brotli.h"

#include <brotli/decode.h>

#include <memory>

#include "util/PerformanceCounters.h"

namespace microbrowser::util {

namespace {

// How much is asked of the decoder per call. Small enough that a stream which
// expands past the ceiling is stopped within one chunk of it rather than after
// allocating everything it wanted, and large enough that a real response is a
// handful of calls.
constexpr std::size_t kChunk = 64 * 1024;

struct StateDeleter {
  void operator()(BrotliDecoderState* state) const { BrotliDecoderDestroyInstance(state); }
};

}  // namespace

bool BrotliInflate(std::span<const std::byte> input, std::size_t max_output,
                   std::vector<std::byte>& out) {
  out.clear();
  if (input.empty()) {
    return false;
  }
  const std::unique_ptr<BrotliDecoderState, StateDeleter> state(
      BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
  if (state == nullptr) {
    return false;
  }

  const std::uint8_t* next_in = reinterpret_cast<const std::uint8_t*>(input.data());
  std::size_t available_in = input.size();
  std::vector<std::uint8_t> chunk(kChunk);

  while (true) {
    std::uint8_t* next_out = chunk.data();
    std::size_t available_out = chunk.size();
    const BrotliDecoderResult result = BrotliDecoderDecompressStream(
        state.get(), &available_in, &next_in, &available_out, &next_out, nullptr);
    const std::size_t produced = chunk.size() - available_out;
    // Checked *before* the bytes are kept, so the ceiling bounds what this
    // function ever holds rather than what it hands back. A brotli stream carries
    // no declared output size -- unlike gzip's ISIZE, which lets a bomb be refused
    // from its own claim -- so this is the only defence there is.
    if (produced > max_output - out.size()) {
      AddPerformanceCounter(PerfCounterId::UtilBrotliRefusals);
      out.clear();
      return false;
    }
    out.insert(out.end(), reinterpret_cast<const std::byte*>(chunk.data()),
               reinterpret_cast<const std::byte*>(chunk.data() + produced));

    switch (result) {
      case BROTLI_DECODER_RESULT_SUCCESS:
        // Trailing bytes after a complete stream are a framing disagreement
        // between the two ends, and accepting them is how one end comes to believe
        // a different document arrived. Refused, the way the HTTP parser refuses
        // trailing bytes after a body.
        if (available_in != 0) {
          AddPerformanceCounter(PerfCounterId::UtilBrotliRefusals);
          out.clear();
          return false;
        }
        AddPerformanceCounter(PerfCounterId::UtilBrotliStreams);
        AddPerformanceCounter(PerfCounterId::UtilBrotliBytesProduced, out.size());
        return true;
      case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
        continue;
      case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
        // There is no more input. A stream that wants more is truncated, and a
        // truncated stream is not a shorter document.
        AddPerformanceCounter(PerfCounterId::UtilBrotliRefusals);
        out.clear();
        return false;
      case BROTLI_DECODER_RESULT_ERROR:
      default:
        AddPerformanceCounter(PerfCounterId::UtilBrotliRefusals);
        out.clear();
        return false;
    }
  }
}

}  // namespace microbrowser::util
