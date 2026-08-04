#include "platform/DescriptorWait.h"

#include <cerrno>
#include <vector>

#include <poll.h>

namespace microbrowser::platform {

bool WaitOnDescriptors(std::span<const util::WaitDescriptor> descriptors,
                       std::int32_t timeout_ms) {
  if (descriptors.empty()) {
    return false;
  }
  std::vector<pollfd> polled;
  polled.reserve(descriptors.size());
  for (const util::WaitDescriptor& descriptor : descriptors) {
    if (!descriptor.IsValid()) {
      continue;
    }
    pollfd entry{};
    entry.fd = descriptor.descriptor;
    entry.events = static_cast<short>((descriptor.readable ? POLLIN : 0) |
                                      (descriptor.writable ? POLLOUT : 0));
    polled.push_back(entry);
  }
  if (polled.empty()) {
    return false;
  }
  const int ready = ::poll(polled.data(), polled.size(), timeout_ms);
  // EINTR is a wakeup, not a failure: the caller comes round the loop and asks
  // again, which is the same thing it does when the timeout expires.
  return ready > 0;
}

}  // namespace microbrowser::platform
