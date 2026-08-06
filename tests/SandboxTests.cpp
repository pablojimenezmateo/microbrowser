// The syscall sandbox.
//
// ADR 0004's mechanism, ADR 0031 §4's policy. **These tests fork**, and that is not an
// inconvenience -- it is the only way to test this: a successful test of a denial is a dead process,
// so the assertion is about a *child's* exit status. A sandbox nobody has watched refuse something is
// a sandbox nobody knows is applied.

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "TestSupport.h"
#include "platform/Sandbox.h"

// **A sanitizer runtime cannot live inside this policy, and that is a fact about the sanitizer.**
// ASan, TSan and UBSan intercept allocation and signals and read `/proc/self/maps` to symbolise a
// report -- so they call `openat`, `rt_sigaction`, `sigaltstack` and more, every one of which a media
// decoder has no business making and which ADR 0031 §4 therefore denies. Under a sanitizer the
// *runtime* trips the filter before the test's own body runs, which is exactly what happened the
// first time this file was run under ASan.
//
// So the forked tests are skipped there rather than the policy widened. Widening it to accommodate a
// build configuration would be putting `openat` back on a decoder's allowlist, which is the one entry
// this whole mechanism exists to remove -- and every browser with a sandbox has the same note: the
// sandbox and the sanitizers are alternative ways to inspect the same process, not simultaneous ones.
// The ordinary build runs all of them, which is where the property is checked.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MICROBROWSER_SANITIZER_RUNTIME 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
#define MICROBROWSER_SANITIZER_RUNTIME 1
#endif
#endif
#if !defined(MICROBROWSER_SANITIZER_RUNTIME)
#define MICROBROWSER_SANITIZER_RUNTIME 0
#endif

namespace microbrowser::tests {

namespace {

#if defined(__linux__)

// What a forked child did: whether it exited normally, with what code, and whether a signal killed
// it. A denial shows up as `SIGSYS`, which is the whole point.
struct ChildOutcome {
  bool exited = false;
  int code = 0;
  bool signalled = false;
  int signal = 0;
};

// Runs `body` in a forked child and reports how it ended. `_exit` rather than `exit` in the child,
// because a confined process cannot run atexit handlers that touch descriptors it no longer may.
template <typename Body>
ChildOutcome RunForked(Body body) {
  const pid_t child = fork();
  if (child == 0) {
    _exit(body());
  }
  ChildOutcome outcome;
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child) {
    return outcome;
  }
  outcome.exited = WIFEXITED(status);
  outcome.code = outcome.exited ? WEXITSTATUS(status) : 0;
  outcome.signalled = WIFSIGNALED(status);
  outcome.signal = outcome.signalled ? WTERMSIG(status) : 0;
  return outcome;
}

#endif

}  // namespace

void RegisterSandboxTests(std::vector<TestCase>& tests) {
#if defined(__linux__)
  AddTest(tests, "Sandbox/AConfinedProcessMayStillDoItsJob", [] {
    // The half that is easy to get wrong in the *other* direction: a filter that refuses what a
    // decoder needs is a decoder that cannot decode. Reading, writing and allocating all have to
    // survive, because every one of the libraries ADR 0031 chose does all three per frame.
    if (!platform::SandboxAvailable() || MICROBROWSER_SANITIZER_RUNTIME) {
      return;
    }
    const ChildOutcome outcome = RunForked([] {
      if (!platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder)) {
        return 3;  // the kernel refused the filter: reported, not silently passed
      }
      // Allocation, which is `mmap`/`brk` underneath and is what a decoder does per frame.
      std::vector<char> buffer(1 << 20, 'x');
      if (buffer[0] != 'x') {
        return 4;
      }
      // A write to an already-open descriptor, which is how a confined process answers.
      const ssize_t written = write(STDERR_FILENO, "", 0);
      return written == 0 ? 0 : 5;
    });
    Expect(outcome.exited, "the child exited rather than dying");
    ExpectEqInt(outcome.code, 0, "and did its work under the filter");
  });

  AddTest(tests, "Sandbox/OpeningAFileKillsTheProcess", [] {
    // **The assertion this file exists for.** ADR 0031 §4 says a decoder has no filesystem: it is
    // handed its descriptors at start-up and opens nothing afterwards. So `open` must not fail -- it
    // must *kill*, because a library that gracefully handles a denial keeps trying, and a
    // compromised one would probe the boundary.
    if (!platform::SandboxAvailable() || MICROBROWSER_SANITIZER_RUNTIME) {
      return;
    }
    const ChildOutcome outcome = RunForked([] {
      if (!platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder)) {
        return 3;
      }
      // `openat` is what glibc actually calls for this, which is the entry a naive allowlist forgets
      // because `open` is the name in the manual.
      const int fd = open("/etc/passwd", O_RDONLY);
      // Unreachable: the process is gone before this returns. If it is reached, the filter let the
      // syscall through, and *that* is the failure this test is looking for.
      if (fd >= 0) {
        close(fd);
        return 1;
      }
      return 2;
    });
    Expect(outcome.signalled, "the child was killed rather than answered");
    ExpectEqInt(outcome.signal, SIGSYS, "by SIGSYS, which is what a seccomp denial is");
    Expect(!outcome.exited || outcome.code != 1,
           "and it certainly did not open the file");
  });

  AddTest(tests, "Sandbox/ForkAndExecAreRefusedBecauseAPolicyMustNotBeEscapable", [] {
    // A decoder that can spawn a process is a decoder that can escape a policy applied to itself:
    // the child would inherit the filter, but a compromised decoder does not need a *clean* child --
    // it needs any process at all, and every one it makes is another thing to probe with.
    if (!platform::SandboxAvailable() || MICROBROWSER_SANITIZER_RUNTIME) {
      return;
    }
    const ChildOutcome outcome = RunForked([] {
      if (!platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder)) {
        return 3;
      }
      const pid_t grandchild = fork();
      if (grandchild == 0) {
        _exit(0);
      }
      return 1;  // unreachable: `clone` is not on the list
    });
    Expect(outcome.signalled && outcome.signal == SIGSYS, "forking is a denial, not an error");
  });

  AddTest(tests, "Sandbox/ASocketIsRefusedSoAConfinedProcessCannotReachTheNetwork", [] {
    // ADR 0031 §3: the decoder never learns where a sample came from and has no reason to know a
    // network exists. This is that sentence as a kernel rule rather than a convention -- and it is
    // the one that matters if a decoder is ever compromised, because exfiltration needs a socket.
    if (!platform::SandboxAvailable() || MICROBROWSER_SANITIZER_RUNTIME) {
      return;
    }
    const ChildOutcome outcome = RunForked([] {
      if (!platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder)) {
        return 3;
      }
      const int fd = static_cast<int>(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0));
      if (fd >= 0) {
        close(fd);
        return 1;
      }
      return 2;
    });
    Expect(outcome.signalled && outcome.signal == SIGSYS, "no socket, by kernel rule");
  });

  AddTest(tests, "Sandbox/TheFilterIsOneWay", [] {
    // A confined process must not be able to widen its own policy, which is what makes "everything it
    // needs is already open" a requirement rather than advice. Applying a second, wider filter is
    // itself a `seccomp` call -- and `seccomp` is not on the list.
    if (!platform::SandboxAvailable() || MICROBROWSER_SANITIZER_RUNTIME) {
      return;
    }
    const ChildOutcome outcome = RunForked([] {
      if (!platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder)) {
        return 3;
      }
      platform::ApplySandbox(platform::SandboxPolicy::MediaDecoder);
      return 1;  // unreachable
    });
    Expect(outcome.signalled && outcome.signal == SIGSYS,
           "a second call to seccomp is refused, so a policy cannot be replaced with a wider one");
  });
#endif

  AddTest(tests, "Sandbox/AHostThatCannotConfineSaysSoRatherThanPretending", [] {
    // ADR 0031 §4's caller contract: a process that cannot confine itself must exit rather than
    // continue, because an unconfined decoder is the thing the process split exists to prevent. That
    // decision needs an honest answer to "can this build confine anything at all", and on a platform
    // without seccomp the answer is a compile-time no rather than a runtime surprise.
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    Expect(platform::SandboxAvailable(), "this build can confine a process");
#else
    Expect(!platform::SandboxAvailable(), "and one that cannot says so");
#endif
  });
}

}  // namespace microbrowser::tests
