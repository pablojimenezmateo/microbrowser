#pragma once

#include <cstddef>

namespace microbrowser::platform {

// The syscall sandbox. ADR 0004's core mechanism, and ADR 0031 §4's requirement stated as code.
//
// **Why this exists before the thing it confines.** ADR 0031 decides that decoders run in a separate
// process because they are the highest-value remote-code-execution target in a browser, and it names
// the policy syscall by syscall. That policy is the security property; the codec library behind it is
// a leaf. So this lands first and on its own, with a test that *actually forks a child and watches a
// denied syscall kill it* -- because a sandbox nobody has watched refuse something is a sandbox
// nobody knows is applied.
//
// It is also not decoder-specific, and that is deliberate. The renderer split (ADR 0004) needs the
// same mechanism with a different list, so the *policy* is a parameter and the enforcement is one
// implementation. A second seccomp filter written later for the renderer would be a second chance to
// leave a hole in.
//
// What a caller must know, because the failure modes are unusual:
//
//   * **It is one-way.** A process that has applied a policy cannot widen it, which is the point --
//     so everything a confined process needs must already be open before the call. A decoder is
//     handed its socket and its shared memory at start-up for exactly this reason.
//   * **A violation kills the process**, it does not return an error. That is chosen rather than
//     inherited: a library that gracefully handles being denied `open` is a library that keeps
//     trying, and a compromised one would probe the boundary. Dying is unambiguous, and ADR 0031 §4
//     already makes a dead decoder an ordinary decode failure.
//   * **It cannot be tested by calling it**, since a successful test of a denial is a dead process.
//     The test forks.
enum class SandboxPolicy {
  // What a media decoder may do: read and write the descriptors it already holds, touch memory it
  // already owns, wait on a futex, and exit. No `open`, no `socket`, no `fork`, no `exec`, no
  // `ptrace` -- and no `openat`, which is the one a naive list forgets because `open` is the name in
  // the manual and `openat` is what libc actually calls.
  MediaDecoder,
};

// Applies the policy to *this* process, permanently. False when the kernel refused -- an old kernel,
// a container that blocks `seccomp` itself, or a `no_new_privs` failure -- and a caller that cannot
// confine itself must **exit rather than continue**: an unconfined decoder is the thing the process
// split exists to prevent, and running one anyway would make the sandbox a claim rather than a fact.
bool ApplySandbox(SandboxPolicy policy);

// Whether this build can confine a process at all. Compiled out on platforms without seccomp, so
// that a caller's "confine or exit" logic is a compile-time decision there rather than a runtime
// surprise.
bool SandboxAvailable();

}  // namespace microbrowser::platform
