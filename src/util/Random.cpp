#include "util/Random.h"

#include <cstdio>
#include <cerrno>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace microbrowser::util {

bool FillRandomBytes(std::span<std::uint8_t> out) {
  if (out.empty()) {
    return true;
  }
#if defined(__linux__)
  std::size_t filled = 0;
  while (filled < out.size()) {
    // `getrandom` may return a short read for a request over 256 bytes, and it may fail with EINTR.
    // Both are ordinary and neither is a reason to produce weaker bytes -- so the loop retries rather
    // than filling the remainder from somewhere else.
    const ssize_t got = ::getrandom(out.data() + filled, out.size() - filled, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;  // fall through to /dev/urandom
    }
    filled += static_cast<std::size_t>(got);
  }
  if (filled == out.size()) {
    return true;
  }
#else
  std::size_t filled = 0;
#endif
  // `/dev/urandom`, for a kernel without `getrandom` or a sandbox that refuses it. Opened per call
  // rather than kept: a retained descriptor is one more thing to close before `main` returns, and a
  // page asking for random bytes is not a hot path.
  std::FILE* device = std::fopen("/dev/urandom", "rbe");
  if (device == nullptr) {
    return false;
  }
  const std::size_t read =
      std::fread(out.data() + filled, 1, out.size() - filled, device);
  std::fclose(device);
  // **No pseudo-random fallback.** If neither source answered, the caller is told so and produces
  // nothing -- because quietly handing back weak bytes is the failure that ships and is never noticed.
  return filled + read == out.size();
}

}  // namespace microbrowser::util
