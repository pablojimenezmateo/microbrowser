#pragma once

#include <cstdint>
#include <span>

#include "util/WaitDescriptor.h"

namespace microbrowser::platform {

// Blocks until one of `descriptors` is ready, or `timeout_ms` elapses.
// A negative timeout means no deadline. True when something became ready.
//
// The only place outside SDL where this process sleeps, and it is here rather
// than in net for the same reason the window is: net produces descriptors and
// has no business knowing how the host waits on them. A future network process
// (ADR 0004) waits with its own copy of this and nothing above it changes.
bool WaitOnDescriptors(std::span<const util::WaitDescriptor> descriptors,
                       std::int32_t timeout_ms);

}  // namespace microbrowser::platform
