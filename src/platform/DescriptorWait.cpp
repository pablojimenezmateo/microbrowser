#include "platform/DescriptorWait.h"

#include <cerrno>
#include <vector>

#include <poll.h>

namespace microbrowser::platform {

bool WaitOnDescriptors(std::span<const util::WaitDescriptor> descriptors,
                       std::int32_t timeout_ms) {
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
    // **Nothing to watch is still a deadline to keep.** The contract above is
    // "blocks until one of `descriptors` is ready, or `timeout_ms` elapses",
    // and returning here made the second half a lie: a caller with no sockets
    // open and a timer pending -- a page that has finished loading and is
    // waiting for a `setTimeout` -- got an instant answer and span.
    //
    // Measured 2026-08-17, `uievents/click/auxclick_event.html` under the WPT
    // runner: **16.6 million turns of the pump loop in ten seconds**, one core
    // at 100%, for a page doing nothing whatever. Roughly 7,000 of the suite's
    // 23,146 tests are expected to time out, and every window-variant one of
    // them burned a full core for its entire deadline. `poll` with no
    // descriptors is the portable sleep, and it is the one this file already
    // depends on.
    //
    // A *negative* timeout means "no deadline", and with nothing to watch that
    // is a wait nothing can ever end. Answering immediately is the only safe
    // reading; blocking forever would turn a caller's bug into a hang.
    if (timeout_ms > 0) {
      ::poll(nullptr, 0, timeout_ms);
    }
    return false;
  }
  const int ready = ::poll(polled.data(), polled.size(), timeout_ms);
  // EINTR is a wakeup, not a failure: the caller comes round the loop and asks
  // again, which is the same thing it does when the timeout expires.
  return ready > 0;
}

}  // namespace microbrowser::platform
