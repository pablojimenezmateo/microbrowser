#include "platform/Sandbox.h"

#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <vector>
#endif

namespace microbrowser::platform {

#if defined(__linux__)

namespace {

// The syscalls a media decoder needs, and nothing else. ADR 0031 §4's list, with the additions a
// real libc forces:
//
//   * `openat` as well as `open` is *absent* -- both are refused -- and the note is here because
//     `open` is the name in the manual while `openat` is what glibc actually calls. A list that
//     refused only `open` would be a sandbox that let a decoder open files.
//   * `mmap`/`munmap`/`mprotect` are allowed because a decoder allocates: every one of these
//     libraries takes memory per frame, and refusing them means refusing to decode. `mprotect` is
//     the uncomfortable one -- it is how W^X is defeated -- and it is allowed because the allocator
//     needs it. Narrowing it by argument is possible and is not attempted here; that is written down
//     rather than glossed.
//   * `rt_sigreturn` is allowed because a process that cannot return from a signal handler cannot
//     be killed cleanly.
//   * `clock_gettime` is allowed because a library that cannot read a clock spins in a backoff loop.
//
// Written as a table so the policy is *readable*, which is the property that matters most: a
// hand-assembled BPF program that nobody can read is a sandbox nobody can review.
constexpr std::array<int, 16> kMediaDecoderSyscalls = {
    SYS_read,
    SYS_write,
    SYS_readv,
    SYS_writev,
    SYS_mmap,
    SYS_munmap,
    SYS_mprotect,
    SYS_brk,
    SYS_futex,
    SYS_exit,
    SYS_exit_group,
    SYS_rt_sigreturn,
    SYS_clock_gettime,
    SYS_getpid,
    SYS_sched_yield,
    SYS_madvise,
};

// The audit arch this build runs on. A filter that did not check it would be a filter a 32-bit
// syscall could walk straight through -- the classic seccomp bypass, because syscall *numbers* differ
// per architecture and a number that is `read` on x86-64 is something else on i386.
#if defined(__x86_64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kAuditArch = AUDIT_ARCH_AARCH64;
#else
constexpr std::uint32_t kAuditArch = 0;
#endif

}  // namespace

bool SandboxAvailable() { return kAuditArch != 0; }

bool ApplySandbox(SandboxPolicy policy) {
  if (!SandboxAvailable()) {
    return false;
  }
  // Every policy is the same mechanism with a different list; today there is one list.
  const std::array<int, 16>& allowed = kMediaDecoderSyscalls;
  (void)policy;

  // `no_new_privs` first, and it is not optional: without it an unprivileged process cannot install
  // a filter at all, and a setuid binary reached later could regain what the filter took away.
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return false;
  }

  std::vector<sock_filter> program;
  // Load the architecture and refuse anything that is not this one, before looking at the number.
  program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                             static_cast<std::uint32_t>(offsetof(seccomp_data, arch))));
  program.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kAuditArch, 1, 0));
  program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
  // Load the syscall number once, then compare it against each allowed value. A linear chain rather
  // than a binary search: the list is sixteen entries, and a readable filter is worth more here than
  // four instructions saved per syscall.
  program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                             static_cast<std::uint32_t>(offsetof(seccomp_data, nr))));
  for (const int number : allowed) {
    program.push_back(
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1));
    program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  }
  // Anything else. **Kill the process rather than return an error**: a library that gracefully
  // handles a denial keeps trying, and a compromised one would probe the boundary.
  program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

  sock_fprog filter{};
  filter.len = static_cast<unsigned short>(program.size());
  filter.filter = program.data();
  // Through `syscall` rather than libseccomp: this is one `prctl`-shaped call and a dependency for it
  // would be a dependency inside the thing being confined.
  return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &filter) == 0;
}

#else

bool SandboxAvailable() { return false; }

bool ApplySandbox(SandboxPolicy) { return false; }

#endif

}  // namespace microbrowser::platform
