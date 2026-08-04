#pragma once

#include <vector>

namespace microbrowser::util {

// One thing the process may block on, and what it is waiting for.
//
// It lives in util rather than in net because it is the vocabulary of a
// *handoff*: net produces these, app consumes them, and platform does the
// actual waiting. None of those three may include the others' headers, and
// putting the type at the bottom of the stack is what lets a socket's readiness
// reach the platform event wait without the app layer learning what a socket is.
//
// Deliberately not a class. There is no invariant to protect and no ownership
// here -- a descriptor is borrowed for the length of one wait, and the thing
// that owns it is the transport that opened it.
struct WaitDescriptor {
  // A file descriptor on every platform this builds for. Negative means
  // "nothing to wait on", which is the answer a transport that never blocks
  // gives -- a scripted one in a test, or a request being served from cache.
  int descriptor = -1;
  // Wake when there is something to read.
  bool readable = false;
  // Wake when it will accept a write. Set while a connection is still being
  // established or while a request body has not gone out in full.
  bool writable = false;

  bool IsValid() const { return descriptor >= 0 && (readable || writable); }

  friend bool operator==(const WaitDescriptor&, const WaitDescriptor&) = default;
};

// What a wait is handed. A vector rather than a fixed array because the bound on
// concurrent requests is a policy decision in net, not a constant the loop knows.
using WaitDescriptorList = std::vector<WaitDescriptor>;

}  // namespace microbrowser::util
